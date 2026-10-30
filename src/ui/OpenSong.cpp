#include "ui/OpenSong.h"

#include "capture/CameraCalibration.h"
#include "core/Log.h"
#include "song/InsertEdits.h"
#include "song/LoopEdits.h"
#include "song/MixEdits.h"
#include "ui/ListeningSettings.h"
#include "ui/TrackRowEdits.h"

#include <algorithm>
#include <ranges>
#include <utility>

namespace overdub::ui
{
namespace
{
// Plugins are prepared for this block size when no device is open.
constexpr int blockSizeWithoutDevice = 512;
} // namespace

OpenSong::OpenSong (std::unique_ptr<song::SongPackage> package, AudioBinding audio, plugin::Instantiator instantiate, ActivityLog* sharedActivityLog)
    : activityLog (sharedActivityLog != nullptr ? *sharedActivityLog : ownActivityLog),
      package_ (std::move (package)),
      inserts (std::move (instantiate),
               [this] (const std::string& hash) { return package_->loadPluginState (hash); },
               package_->song().sampleRate,
               blockSizeOf (audio)),
      playback (package_->location()),
      listening_ (package_->location(), package_->song(), [this] { reloadEngine(); })
{
    package_->setPluginStateCapture ([this] (song::Song& song) { capturePluginState (song); });
    inserts.sync (song());
    attachAudio (std::move (audio));
    listening_.follow (song()); // starts the missing Transpose renders
}

OpenSong::~OpenSong()
{
    for (const auto& error : detachAudio())
        core::logError (error);
    // Closing the Song waits for a movie still completing (ADR 0003, issue #90 amendment).
    if (auto* pass = std::get_if<PassActivity> (&activity))
        for (const auto& error : finishPass (pass->recorder->finish()))
            core::logError (error);
    if (camera != nullptr)
        camera->close();
    if (inserts.takeStateChanged())
        package_->changeWithoutHistory (song());
    if (auto written = package_->flush(); ! written.has_value())
        core::logError (written.error());
    package_->setPluginStateCapture ({});
}

int OpenSong::blockSizeOf (const AudioBinding& audio)
{
    return audio.engine != nullptr ? audio.engine->configuration().maxBlockSize : blockSizeWithoutDevice;
}

void OpenSong::capturePluginState (song::Song& song)
{
    // Failures are logged per slot by the host; the change goes through regardless.
    inserts.captureStates (song, [this] (const juce::MemoryBlock& state) { return package_->storePluginState (state); });
}

void OpenSong::attachAudio (AudioBinding audio)
{
    rateMismatch.reset();
    if (audio.engine != nullptr && audio.sampleRate > 0 && audio.sampleRate != song().sampleRate)
    {
        // The Song's rate is fixed (ADR 0004): played at another rate it would change speed and pitch.
        rateMismatch = core::Error { "ui.sample_rate_mismatch",
                                     "the audio device runs at another rate than the Song",
                                     { { "song_rate", song().sampleRate },
                                       { "device_rate", audio.sampleRate },
                                       { "rate_offered", std::ranges::contains (audio.offeredSampleRates, song().sampleRate) } } };
        audio.engine = nullptr;
    }
    binding = std::move (audio);
    if (binding.engine == nullptr)
    {
        // Nothing renders the plugins without an engine: an export renders them at the Song's rate.
        if (inserts.preparedSampleRate() != song().sampleRate)
            inserts.prepare (song().sampleRate, blockSizeOf (binding));
        return;
    }
    // Invariant (see attachAudio in OpenSong.h): no engine renders the plugins now.
    inserts.prepare (song().sampleRate, blockSizeOf (binding));
    binding.engine->stop();
    binding.engine->setMonitoring (true); // which Tracks are heard is decided by `monitored`
    reloadEngine();
    binding.engine->seek (song().settings.playhead);
}

std::vector<core::Error> OpenSong::detachAudio()
{
    // The export renders the plugins the next engine is prepared for: it must stop first.
    if (auto* exporting = std::get_if<ExportActivity> (&activity))
    {
        exporting->run->stop ("audio_detached");
        endActivity ("ui.export_stopped");
    }
    std::vector<core::Error> errors;
    if (auto* pass = std::get_if<PassActivity> (&activity); pass != nullptr && binding.engine != nullptr)
    {
        binding.engine->stop();
        if (auto finished = pass->recorder->release()) // a movie still completing: poll() finishes the Pass
            errors = finishPass (std::move (*finished));
    }
    if (binding.engine != nullptr)
    {
        binding.engine->stop();
        binding.engine->loadSong (song::makeEmptySong (song().sampleRate), package_->location());
    }
    binding = AudioBinding {};
    rateMismatch.reset();
    return errors;
}

std::optional<core::Error> OpenSong::audioProblem() const
{
    if (binding.engine != nullptr)
        return std::nullopt;
    if (rateMismatch.has_value())
        return rateMismatch;
    return core::Error { "ui.no_audio_device", "no audio device is open to record from", {} };
}

bool OpenSong::isPlaying() const
{
    return binding.engine != nullptr && binding.engine->isPlaying();
}

song::SampleCount OpenSong::position() const
{
    return binding.engine != nullptr ? binding.engine->position() : song().settings.playhead;
}

video::PlaybackClock OpenSong::playbackClock() const
{
    video::PlaybackClock clock;
    clock.sampleRate = song().sampleRate;
    clock.position = position();
    clock.outputLatencySamples = binding.latency.outputLatencySamples;
    if (binding.engine != nullptr)
    {
        clock.playing = binding.engine->isPlaying();
        clock.anchor = binding.engine->latestAnchor();
    }
    return clock;
}

bool OpenSong::isSavingPass() const
{
    const auto* pass = std::get_if<PassActivity> (&activity);
    return pass != nullptr && pass->recorder->phase() == capture::PassPhase::saving;
}

int OpenSong::recordingPassNumber() const
{
    const auto* pass = std::get_if<PassActivity> (&activity);
    return pass != nullptr ? pass->number : 0;
}

core::Expected<void> OpenSong::record()
{
    if (isRecording())
        return {};
    if (binding.engine == nullptr)
        return std::unexpected (*audioProblem());
    return startPass (binding.engine->position(), binding.engine->isPlaying());
}

core::Expected<void> OpenSong::startPass (song::SampleCount recordPoint, bool playing)
{
    if (! nothingRuns())
        return {};
    const auto passNumber = nextPassNumber (song());
    const auto preRollSamples = (song::SampleCount) song::preRollSeconds (song().settings.preRoll) * song().sampleRate;
    const auto usePreRoll = ! playing && preRollSamples > 0;

    capture::PassRecorderOptions options;
    if (usePreRoll)
        options.recordPoint = recordPoint;
    if (song().settings.cameraArmed && camera != nullptr && camera->openCamera().has_value())
    {
        options.camera = camera;
        options.cameraCalibrationSamples = cameraCalibrationSamples (camera->openCamera()->id);
        options.outputDevice = binding.outputDeviceName;
    }
    auto started = capture::PassRecorder::start (*package_, *binding.engine, binding.latency, options);
    if (! started.has_value())
        return std::unexpected (started.error());

    beginActivity (PassActivity { .recorder = std::move (*started), .number = passNumber, .preRollPoint = options.recordPoint });
    // A Mixer preview still applied (a drag the Pass interrupts) gives way to the Song's mix.
    binding.engine->setMix (heard().song);
    if (usePreRoll)
    {
        const auto playFrom = std::max<song::SampleCount> (0, recordPoint - preRollSamples);
        binding.engine->seek (playFrom);
        core::logEvent (core::Level::info, "ui.pre_roll_started", { { "record_point", recordPoint }, { "play_from", playFrom }, { "pre_roll_seconds", song::preRollSeconds (song().settings.preRoll) } });
    }
    if (! playing)
        binding.engine->play();
    return {};
}

core::Expected<void> OpenSong::recordAgain()
{
    if (binding.engine == nullptr)
        return std::unexpected (*audioProblem());
    if (isRecording())
    {
        // The Pass may already have ended on the audio thread, its start gone with it: poll() reads
        // the record point from the Pass once it is finished.
        recordAgainAfterPass = true;
        stop();
        return {};
    }
    // MainComponent::run refuses record_again while the Song has no Pass (ui/Availability.h).
    if (! OVERDUB_CHECK (! song().passes.empty(), "ui.record_again_without_pass"))
        return {};
    return recordAgainFrom (song().passes.back().startSample);
}

core::Expected<void> OpenSong::recordAgainFrom (song::SampleCount recordPoint)
{
    binding.engine->stop();
    binding.engine->seek (recordPoint);
    const core::Fields fields { { "record_point", recordPoint }, { "pass_id", song().passes.back().id } };
    // The engine still reports playing until its next block: the transport is stopped all the same.
    auto started = startPass (recordPoint, false);
    if (started.has_value())
        core::logEvent (core::Level::info, "ui.record_again", fields);
    return started;
}

std::optional<song::SampleCount> OpenSong::preRollRecordPoint() const
{
    const auto* pass = std::get_if<PassActivity> (&activity);
    if (pass == nullptr || pass->recorder->phase() != capture::PassPhase::waiting)
        return std::nullopt;
    return pass->preRollPoint;
}

std::optional<song::SampleCount> OpenSong::runningPassStart() const
{
    const auto* pass = std::get_if<PassActivity> (&activity);
    return pass != nullptr ? pass->recorder->runningPassStart() : std::nullopt;
}

core::Expected<void> OpenSong::setPreRoll (song::PreRoll preRoll)
{
    if (preRoll == song().settings.preRoll)
        return {};
    auto changed = song();
    changed.settings.preRoll = preRoll;
    package_->changeWithoutHistory (std::move (changed));
    return {};
}

core::Expected<void> OpenSong::setPracticeSpeed (int speedPercent)
{
    if (auto checked = song::checkPracticeSpeed (speedPercent); ! checked.has_value())
        return std::unexpected (std::move (checked.error()));
    if (auto withLoopSpeed = listening_.setPracticeSpeed (song(), speedPercent))
    {
        package_->changeWithoutHistory (std::move (*withLoopSpeed));
        core::logEvent (core::Level::info, "ui.loop_practice_speed_set", { { "loop_id", song().settings.activeLoopId.value_or ("") }, { "speed_percent", speedPercent } });
    }
    listening_.follow (song());
    return {};
}

bool OpenSong::practiceSpeedMayChange() const
{
    // A Pass keeps the Song's playback as it started, and a Separation or a conversion its Song.
    const auto kind = kindOf (activity);
    return kind == ActivityKind::idle || kind == ActivityKind::exportSong;
}

core::Expected<void> OpenSong::changeSettings (core::Expected<song::Song> changed)
{
    if (! changed.has_value())
        return std::unexpected (std::move (changed.error()));
    if (*changed != song())
        package_->changeWithoutHistory (std::move (*changed));
    return {};
}

void OpenSong::setTranscribing (bool on)
{
    if (on == listening_.isTranscribing())
        return;
    const auto before = heard();
    listening_.setTranscribing (song(), on, practiceSpeedMayChange());
    hearChange (before);
}

core::Expected<void> OpenSong::setActiveLoop (const std::optional<song::Id>& loopId, bool looping)
{
    auto changed = changeHeardLoop (song::setActiveLoop (song(), loopId, looping));
    if (changed.has_value())
        core::logEvent (core::Level::info, "ui.active_loop_set", { { "loop_id", loopId.value_or ("") }, { "looping", song().settings.looping } });
    return changed;
}

core::Expected<void> OpenSong::setLooping (bool looping)
{
    auto changed = changeHeardLoop (song::setLooping (song(), looping));
    if (changed.has_value())
        core::logEvent (core::Level::info, "ui.looping_set", { { "looping", looping } });
    return changed;
}

core::Expected<void> OpenSong::changeHeardLoop (core::Expected<song::Song> changed)
{
    if (! changed.has_value())
        return std::unexpected (std::move (changed.error()));
    const auto before = heard();
    if (*changed != song())
        package_->changeWithoutHistory (std::move (*changed));
    listening_.hearLoopPracticeSpeed (song(), practiceSpeedMayChange());
    hearChange (before);
    return {};
}

void OpenSong::hearChange (const Heard& before)
{
    const auto after = heard();
    const auto sameShift = before.octaveShiftedTrack == after.octaveShiftedTrack && before.octaveShift == after.octaveShift;
    const auto mixApplied = binding.engine != nullptr && sameShift && song::differsOnlyInMix (before.song, after.song) && binding.engine->setMix (after.song);
    if (mixApplied)
    {
        binding.engine->setLoop (after.loop);
        binding.engine->setBandFilter (after.bandFilter);
    }
    else
    {
        reloadEngine(); // syncs the InsertHost, also without an engine
    }
    listening_.follow (song());
}

core::Expected<void> OpenSong::changeListening (const intent::ListeningSetting& setting)
{
    auto changed = withListeningSetting (song(), setting);
    if (! changed.has_value())
        return std::unexpected (std::move (changed.error()));
    if (*changed == song())
        return {};
    const auto before = heard();
    package_->changeWithoutHistory (std::move (*changed));
    hearChange (before);
    logListeningSet (setting);
    return {};
}

void OpenSong::stop()
{
    if (binding.engine == nullptr)
        return;
    if (auto* pass = std::get_if<PassActivity> (&activity))
        pass->recorder->requestStop();
    binding.engine->stop();
}

void OpenSong::playOrStop()
{
    if (binding.engine == nullptr)
        return;
    recordAgainAfterPass = false; // Space after Record Again stops for good
    if (isRecording() || binding.engine->isPlaying())
    {
        stop();
        return;
    }
    if (const auto loop = heard().loop; loop.has_value())
    {
        const auto position = binding.engine->position();
        if (position < loop->start || position >= loop->end)
            binding.engine->seek (loop->start);
    }
    binding.engine->play();
}

void OpenSong::rewind()
{
    if (binding.engine == nullptr)
        return;
    if (auto* pass = std::get_if<PassActivity> (&activity))
        pass->recorder->requestStop();
    binding.engine->seek (0);
}

void OpenSong::seek (song::SampleCount sample)
{
    const auto position = std::max<song::SampleCount> (0, sample);
    if (binding.engine != nullptr)
    {
        binding.engine->seek (position);
        return;
    }
    auto changed = song();
    changed.settings.playhead = position;
    package_->changeWithoutHistory (std::move (changed));
}

core::Expected<void> OpenSong::edit (song::Song changed, std::string label)
{
    const auto before = heard();
    auto result = package_->edit (std::move (changed), std::move (label));
    updateEngine (before);
    return result;
}

core::Expected<song::Id> OpenSong::addInsert (const song::Id& trackId, const plugin::AvailableAudioUnit& plugin)
{
    const song::InsertSlot slot { .id = song::makeId(), .component = plugin.component, .name = plugin.name, .version = plugin.version };
    auto changed = song::addInsert (song(), trackId, slot);
    if (! changed.has_value())
        return std::unexpected (changed.error());
    if (auto edited = edit (std::move (*changed), song::addInsertLabel (song(), trackId, plugin.name)); ! edited.has_value())
        return std::unexpected (edited.error());
    return slot.id;
}

void OpenSong::notePluginStateChanged()
{
    package_->changeWithoutHistory (song());
}

void OpenSong::previewMix (const song::Song& candidate)
{
    if (binding.engine != nullptr && song::differsOnlyInMix (song(), candidate))
        binding.engine->setMix (listening_.withHeardMix (candidate));
}

engine::MeterPeaks OpenSong::meterPeaks()
{
    return binding.engine != nullptr ? binding.engine->takeMeterPeaks() : engine::MeterPeaks {};
}

const song::Peaks& OpenSong::peaks (const song::ClipSource& source)
{
    if (const auto cached = peaksByMediaId.find (source.id); cached != peaksByMediaId.end())
        return cached->second;

    auto loaded = package_->peaks (source);
    if (! loaded.has_value())
        core::logError (loaded.error(), core::Level::warn);
    return peaksByMediaId[source.id] = loaded.value_or (song::Peaks {});
}

core::Expected<void> OpenSong::undo()
{
    const auto before = heard();
    auto result = package_->undo();
    updateEngine (before);
    return result;
}

core::Expected<void> OpenSong::redo()
{
    const auto before = heard();
    auto result = package_->redo();
    updateEngine (before);
    return result;
}

core::Expected<void> OpenSong::setArmed (const song::Id& trackId, bool armed)
{
    package_->changeWithoutHistory (withArmed (song(), trackId, armed));
    reloadEngine();
    return {};
}

core::Expected<void> OpenSong::setInputChannels (const song::Id& trackId, std::vector<int> inputChannels)
{
    package_->changeWithoutHistory (withInputChannels (song(), trackId, std::move (inputChannels)));
    reloadEngine();
    return {};
}

void OpenSong::setMonitoring (const song::Id& trackId, bool on)
{
    if (on)
        monitored.insert (trackId);
    else
        monitored.erase (trackId);
    reloadEngine();
}

std::vector<core::Expected<song::Id>> OpenSong::importFiles (const std::vector<juce::File>& files)
{
    auto results = package_->importBackingTracks (files);
    reloadEngine();
    listening_.follow (song());
    return results;
}

core::Expected<void> OpenSong::startSeparation (const song::Id& trackId, song::SongPackage::Clock::time_point now)
{
    if (isSeparating() || ! nothingRuns())
        return {};
    auto started = song::SeparationRun::start (*package_, trackId, separationTools, now);
    if (! started.has_value())
        return std::unexpected (started.error());
    beginActivity (SeparationActivity { std::move (*started) });
    if (binding.engine != nullptr)
        binding.engine->stop();
    return {};
}

void OpenSong::cancelSeparation (song::SongPackage::Clock::time_point now)
{
    auto* separation = std::get_if<SeparationActivity> (&activity);
    if (separation == nullptr)
        return;
    separation->run->cancel (now);
    endActivity ("song.separation_cancelled");
    reloadEngine();
}

song::SeparationProgress OpenSong::separationProgress() const
{
    const auto* separation = std::get_if<SeparationActivity> (&activity);
    return separation != nullptr ? separation->run->progress() : song::SeparationProgress {};
}

std::string OpenSong::separationSourceName() const
{
    const auto* separation = std::get_if<SeparationActivity> (&activity);
    return separation != nullptr ? separation->run->sourceName() : std::string();
}

std::optional<core::Error> OpenSong::advanceSeparation (song::SongPackage::Clock::time_point now)
{
    auto* separation = std::get_if<SeparationActivity> (&activity);
    if (separation == nullptr)
        return std::nullopt;
    auto ended = separation->run->advance (now);
    if (! ended.has_value())
        return std::nullopt;
    endActivity (ended->has_value() ? std::nullopt : std::optional<std::string> (ended->error().code));
    reloadEngine();
    if (! ended->has_value())
        return ended->error();
    listening_.follow (song());
    return std::nullopt;
}

core::Expected<void> OpenSong::startSampleRateConversion (int toRate, song::SongPackage::Clock::time_point now)
{
    if (isConverting() || ! nothingRuns())
        return {};
    auto started = song::SampleRateConversionRun::start (*package_, toRate, now);
    if (! started.has_value())
        return std::unexpected (started.error());
    beginActivity (ConversionActivity { std::move (*started) });
    if (binding.engine != nullptr)
        binding.engine->stop();
    return {};
}

void OpenSong::cancelSampleRateConversion (song::SongPackage::Clock::time_point now)
{
    auto* conversion = std::get_if<ConversionActivity> (&activity);
    if (conversion == nullptr)
        return;
    conversion->run->cancel (now);
    endActivity ("song.conversion_cancelled");
}

double OpenSong::conversionFraction() const
{
    const auto* conversion = std::get_if<ConversionActivity> (&activity);
    return conversion != nullptr ? conversion->run->fraction() : 0.0;
}

int OpenSong::conversionTargetRate() const
{
    const auto* conversion = std::get_if<ConversionActivity> (&activity);
    return conversion != nullptr ? conversion->run->toRate() : 0;
}

std::optional<core::Error> OpenSong::advanceConversion (song::SongPackage::Clock::time_point now)
{
    auto* conversion = std::get_if<ConversionActivity> (&activity);
    if (conversion == nullptr)
        return std::nullopt;
    const auto before = heard();
    auto ended = conversion->run->advance (now);
    if (! ended.has_value())
        return std::nullopt;
    endActivity (ended->has_value() ? std::nullopt : std::optional<std::string> (ended->error().code));
    updateEngine (before);
    if (! ended->has_value())
        return ended->error();
    return std::nullopt;
}

std::vector<core::Error> OpenSong::poll (song::SongPackage::Clock::time_point now)
{
    auto errors = handleCrashes();
    for (auto& error : listening_.advance (song(), now))
        errors.push_back (std::move (error));
    for (auto& error : handleCameraChanges())
        errors.push_back (std::move (error));
    if (auto* exporting = std::get_if<ExportActivity> (&activity))
        if (auto error = endExportIfEnded (exporting->run->advance (now, audioLetGoOfPlugins())))
            errors.push_back (std::move (*error));
    if (auto* pass = std::get_if<PassActivity> (&activity))
        if (auto finished = pass->recorder->poll (now))
            for (auto& error : finishPass (std::move (*finished)))
                errors.push_back (std::move (error));
    if (recordAgainAfterPass && ! isRecording())
    {
        recordAgainAfterPass = false;
        // A Pass that captured nothing added none: the one before it is the most recent.
        if (! song().passes.empty())
            if (auto started = recordAgainFrom (song().passes.back().startSample); ! started.has_value())
                errors.push_back (started.error());
    }
    if (auto error = advanceSeparation (now))
        errors.push_back (std::move (*error));
    if (auto error = advanceConversion (now))
        errors.push_back (std::move (*error));

    // A crashed plugin reloads only once no Pass or export runs (a Pass keeps its plan anyway).
    if (! isRecording() && ! isExporting())
        inserts.reloadCrashed (song());
    // During a Pass the plan stays as it started; finishPass rebuilds it.
    if (! isRecording() && inserts.takeGraphChanged())
        reloadEngine();
    if (inserts.takeStateChanged())
        package_->changeWithoutHistory (song(), now);

    if (! isRecording() && binding.engine != nullptr && ! binding.engine->isPlaying())
    {
        const auto position = binding.engine->position();
        if (position != song().settings.playhead)
        {
            auto changed = song();
            changed.settings.playhead = position;
            package_->changeWithoutHistory (std::move (changed), now);
        }
    }

    if (auto written = package_->writeIfDue (now); ! written.has_value())
        errors.push_back (written.error());
    return errors;
}

std::vector<core::Error> OpenSong::handleCrashes()
{
    const auto crashes = inserts.takeCrashes();
    if (crashes.empty())
        return {};

    std::vector<core::Error> errors;
    if (auto* exporting = std::get_if<ExportActivity> (&activity))
        if (auto error = endExportIfEnded (exporting->run->crashed (crashes)))
            errors.push_back (std::move (*error));

    core::Fields reloading { { "slot_ids", nlohmann::json::array() }, { "names", nlohmann::json::array() } };
    core::Fields givenUp = reloading;
    for (const auto& crash : crashes)
    {
        auto& fields = crash.willReload ? reloading : givenUp;
        fields["slot_ids"].push_back (crash.slotId);
        fields["names"].push_back (crash.name);
    }
    if (! reloading["slot_ids"].empty())
        errors.push_back (core::Error { "ui.plugin_crashed", "a plugin crashed and is bypassed until it reloads", reloading });
    if (! givenUp["slot_ids"].empty())
        errors.push_back (core::Error { "ui.plugin_crashed_repeatedly", "a plugin keeps crashing and stays bypassed", givenUp });
    return errors;
}

std::vector<core::Error> OpenSong::finishPass (capture::FinishedPass finished)
{
    const auto deviceLost = finished.stopReason == engine::RecordStopReason::deviceStopped;
    const auto& pass = std::get<PassActivity> (activity);
    const auto passNumber = pass.number;
    const auto cameraLost = pass.cameraLost;

    std::vector<core::Error> errors;
    // The rest of the performance was not recorded: an Alert, shown once the Pass has ended.
    if (deviceLost)
        errors.push_back (core::Error { "ui.audio_device_lost_during_pass",
                                        "the audio device stopped during the Pass",
                                        { { "pass_number", passNumber }, { "kept", finished.pass.has_value() } } });
    if (! finished.pass.has_value())
    {
        // A Pass the device stopped before it began captured nothing: the Alert above says why.
        if (! (deviceLost && finished.pass.error().code == "capture.pass_empty"))
            errors.push_back (finished.pass.error());
    }
    else
    {
        for (auto& notice : finished.pass->notices) // the Pass stands; its Video Take may not
            errors.push_back (std::move (notice));
    }

    // A Pass that stood ended without an Error, whatever Notices it raised.
    endActivity (finished.pass.has_value() ? std::nullopt : std::optional<std::string> (errors.front().code));
    if (cameraLost)
        disarmLostCamera();
    reloadEngine();
    return errors;
}

bool OpenSong::nothingRuns() const
{
    return OVERDUB_CHECK (std::holds_alternative<Idle> (activity), "ui.activity_already_running", { "kind", std::string (activityKindName (activityKind())) });
}

void OpenSong::beginActivity (Activity started)
{
    activity = std::move (started);
    activityCounter = activityLog.start();
}

void OpenSong::endActivity (std::optional<std::string> errorCode)
{
    const auto kind = kindOf (activity);
    activity = Idle {};
    activityLog.finish (kind, activityCounter, std::move (errorCode));
}

//==============================================================================
// Camera

core::Expected<void> OpenSong::attachCamera (capture::Camera* newCamera, juce::File calibrationFile)
{
    if (camera != nullptr && camera != newCamera)
        camera->close();
    camera = newCamera;
    cameraCalibrationFile = std::move (calibrationFile);
    cameras = camera != nullptr ? camera->devices() : std::vector<capture::CameraDevice> {};
    return syncCamera();
}

std::vector<core::Error> OpenSong::handleCameraChanges()
{
    if (camera == nullptr || ! camera->takeDevicesChanged())
        return {};
    cameras = camera->devices();
    core::logEvent (core::Level::info, "ui.cameras_changed", { { "cameras", cameras.size() } });

    auto* pass = std::get_if<PassActivity> (&activity);
    const auto open = camera->openCamera();
    if (! song().settings.cameraArmed || ! open.has_value() || (pass != nullptr && pass->cameraLost) || std::ranges::contains (cameras, open->id, &capture::CameraDevice::id))
        return {};

    // The Armed camera is gone. A running Pass keeps its movie (written until the camera went) and
    // records on; the camera is disarmed once the Pass is finished.
    core::Error lost { "ui.camera_disconnected", "the Armed camera was disconnected", { { "camera_id", open->id }, { "during_pass", pass != nullptr } } };
    if (pass != nullptr)
        pass->cameraLost = true;
    else
        disarmLostCamera();
    return { lost };
}

void OpenSong::disarmLostCamera()
{
    if (camera != nullptr)
        camera->close();
    if (! song().settings.cameraArmed)
        return;
    // The choice stays: the camera is offered again when it comes back, and arming reopens it.
    auto changed = song();
    changed.settings.cameraArmed = false;
    package_->changeWithoutHistory (std::move (changed));
}

core::Expected<void> OpenSong::setCameraArmed (bool armed)
{
    if (camera == nullptr || armed == song().settings.cameraArmed)
        return {};
    auto changed = song();
    changed.settings.cameraArmed = armed;
    package_->changeWithoutHistory (std::move (changed));
    cameras = camera->devices();
    return syncCamera();
}

core::Expected<void> OpenSong::setCameraDevice (const std::string& cameraId)
{
    if (cameraId == song().settings.cameraDeviceId)
        return {};
    auto changed = song();
    changed.settings.cameraDeviceId = cameraId;
    package_->changeWithoutHistory (std::move (changed));
    if (camera == nullptr)
        return {};
    cameras = camera->devices();
    if (song().settings.cameraArmed)
        camera->close(); // reopened on the new choice
    return syncCamera();
}

void* OpenSong::cameraPreviewLayer() const
{
    if (camera == nullptr || ! song().settings.cameraArmed || ! camera->openCamera().has_value())
        return nullptr;
    return camera->previewLayer();
}

core::Expected<void> OpenSong::syncCamera()
{
    if (camera == nullptr)
        return {};
    if (! song().settings.cameraArmed)
    {
        camera->close();
        return {};
    }
    if (const auto open = camera->openCamera(); open.has_value() && open->id == song().settings.cameraDeviceId)
        return {};

    auto opened = camera->open (song().settings.cameraDeviceId);
    auto changed = song();
    if (! opened.has_value())
    {
        changed.settings.cameraArmed = false;
        package_->changeWithoutHistory (std::move (changed));
        return std::unexpected (opened.error());
    }
    // The default camera opened for no (or a missing) choice becomes the choice.
    if (! song().settings.cameraDeviceId.empty() && opened->id != song().settings.cameraDeviceId)
        core::logEvent (core::Level::warn, "ui.camera_fallback", { { "chosen_camera_id", song().settings.cameraDeviceId }, { "opened_camera_id", opened->id } });
    changed.settings.cameraDeviceId = opened->id;
    if (changed != song())
        package_->changeWithoutHistory (std::move (changed));
    return {};
}

capture::SampleCount OpenSong::cameraCalibrationSamples (const std::string& cameraId) const
{
    const auto offsetMs = capture::readCameraCalibration (cameraCalibrationFile, { cameraId, binding.outputDeviceName });
    if (! offsetMs.has_value())
        core::logError (offsetMs.error(), core::Level::warn); // placed without calibration
    return capture::calibrationSamples (offsetMs.value_or (0.0), song().sampleRate);
}

void OpenSong::updateEngine (const Heard& before)
{
    if (before.song.sampleRate != song().sampleRate)
    {
        // The device runs at the old rate: the owner opens it again at the new one.
        core::logEvent (core::Level::info, "ui.song_rate_changed", { { "from_rate", before.song.sampleRate }, { "to_rate", song().sampleRate } });
        for (const auto& error : detachAudio())
            core::logError (error);
        sampleRateChanged = true;
        return;
    }
    hearChange (before);
}

void OpenSong::reloadEngine()
{
    if (auto* exporting = std::get_if<ExportActivity> (&activity))
    {
        exporting->run->followSong();
        return;
    }
    inserts.sync (song());
    if (binding.engine == nullptr)
        return;
    const auto heardNow = heard();
    binding.engine->loadSong (heardNow.song, package_->location(), inserts.insertFactory(), monitored, listening_.practicePlayback (song()));
    binding.engine->setLoop (heardNow.loop);
    binding.engine->setBandFilter (heardNow.bandFilter);
}

core::Expected<void> OpenSong::startExport (exporting::ExportRequest request, song::SongPackage::Clock::time_point now)
{
    if (isExporting() || ! nothingRuns())
        return {};
    std::optional<exporting::MovieSource> movie;
    if (request.format == exporting::ExportFormat::mp4)
    {
        const auto& videoTakes = song().videoTakes;
        const auto found = request.videoTakeId.has_value() ? std::ranges::find (videoTakes, *request.videoTakeId, &song::VideoTake::id) : videoTakes.end();
        if (found == videoTakes.end())
            return std::unexpected (core::Error { "ui.no_video_take", "an MP4 needs a Video Take of the Song", { { "video_take_id", request.videoTakeId.value_or ("") } } });
        movie = exporting::MovieSource { package_->location().getChildFile (found->file), *found };
    }
    if (auto folder = exporting::checkExportFolder (request.destination); ! folder.has_value())
        return std::unexpected (std::move (folder.error()));

    lastExport.reset();
    if (binding.engine != nullptr)
    {
        binding.engine->stop();
        binding.engine->loadSong (song::makeEmptySong (song().sampleRate), package_->location());
    }
    core::Fields fields { { "format", exporting::formatName (request.format) }, { "path", request.destination.getFullPathName().toStdString() }, { "audio", binding.engine != nullptr } };
    if (movie.has_value())
    {
        fields["video_take_id"] = movie->videoTake.id;
        fields["length"] = exporting::videoLengthName (request.videoLength);
    }
    core::logEvent (core::Level::info, "ui.export_requested", fields);
    auto run = std::make_unique<ExportRun> (std::move (request), std::move (movie), *package_, inserts, blockSizeOf (binding), now);
    auto& started = *run;
    beginActivity (ExportActivity { std::move (run) });
    // Without audio nothing else renders the plugins: the export starts now.
    if (auto error = endExportIfEnded (started.advance (now, audioLetGoOfPlugins())))
        return std::unexpected (std::move (*error));
    return {};
}

std::optional<core::Error> OpenSong::cancelExport()
{
    auto* exporting = std::get_if<ExportActivity> (&activity);
    if (exporting == nullptr)
        return std::nullopt;
    return endExportIfEnded (exporting->run->cancel());
}

ExportPhase OpenSong::exportPhase() const
{
    const auto* exporting = std::get_if<ExportActivity> (&activity);
    return exporting != nullptr ? exporting->run->phase() : ExportPhase::idle;
}

double OpenSong::exportProgress() const
{
    const auto* exporting = std::get_if<ExportActivity> (&activity);
    return exporting != nullptr ? exporting->run->progress() : 0.0;
}

bool OpenSong::audioLetGoOfPlugins() const
{
    return binding.engine == nullptr || ! binding.engine->hasPendingPlan();
}

std::optional<core::Error> OpenSong::endExportIfEnded (std::optional<ExportEnd> end)
{
    if (! end.has_value())
        return std::nullopt;
    endActivity (end->error.has_value() ? std::optional<std::string> (end->error->code) : std::nullopt);
    reloadEngine();
    lastExport = std::move (end->file);
    return std::move (end->error);
}
} // namespace overdub::ui
