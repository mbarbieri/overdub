#include "ui/OpenSong.h"

#include "capture/CameraCalibration.h"
#include "core/Log.h"
#include "song/InsertEdits.h"
#include "song/LoopEdits.h"
#include "song/MixEdits.h"
#include "song/TranscribeMix.h"
#include "song/Transpose.h"
#include "ui/TrackRowEdits.h"

#include <algorithm>
#include <ranges>
#include <utility>

namespace overdub::ui
{
namespace
{
// The run renders a Backing Track the Song no longer has (an undone import or Separation).
bool rendersRemovedBackingTrack (const song::TransposeRun& run, const song::Song& song)
{
    return std::ranges::any_of (run.renderJobs(), [&song] (const song::TransposeRenderJob& job) { return ! std::ranges::contains (song.backingTracks, job.backingTrackId, &song::BackingTrack::id); });
}

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
      // The derived audio plays until every render of the Song's Transpose is there.
      heardShift (song::missingTransposeRenders (package_->location(), package_->song()).empty() ? package_->song().transpose : song::Transpose {})
{
    practiceFolder = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("overdub-practice-" + juce::Uuid().toString());
    package_->setPluginStateCapture ([this] (song::Song& song) { capturePluginState (song); });
    song::deleteTransposeRendersExcept (package_->location(), package_->song().transpose);
    inserts.sync (song());
    attachAudio (std::move (audio));
    advanceTranspose (song::SongPackage::Clock::now()); // starts the missing renders; nothing has failed yet
}

OpenSong::~OpenSong()
{
    for (const auto& error : detachAudio())
        core::logError (error);
    octaveRun.reset();
    practiceRun.reset();
    practiceFolder.deleteRecursively();
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
    binding.engine->seek (song().playhead);
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
    return binding.engine != nullptr ? binding.engine->position() : song().playhead;
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
    if (auto refused = refusal (activity, Gesture::record))
        return std::unexpected (std::move (*refused));
    if (binding.engine == nullptr)
        return std::unexpected (*audioProblem());
    return startPass (binding.engine->position(), binding.engine->isPlaying());
}

core::Expected<void> OpenSong::startPass (song::SampleCount recordPoint, bool playing)
{
    const auto passNumber = nextPassNumber (song());
    const auto preRollSamples = (song::SampleCount) song::preRollSeconds (song().preRoll) * song().sampleRate;
    const auto usePreRoll = ! playing && preRollSamples > 0;

    capture::PassRecorderOptions options;
    if (usePreRoll)
        options.recordPoint = recordPoint;
    if (song().cameraArmed && camera != nullptr && camera->openCamera().has_value())
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
    binding.engine->setMix (heardSong());
    if (usePreRoll)
    {
        const auto playFrom = std::max<song::SampleCount> (0, recordPoint - preRollSamples);
        binding.engine->seek (playFrom);
        core::logEvent (core::Level::info, "ui.pre_roll_started", { { "record_point", recordPoint }, { "play_from", playFrom }, { "pre_roll_seconds", song::preRollSeconds (song().preRoll) } });
    }
    if (! playing)
        binding.engine->play();
    return {};
}

core::Expected<void> OpenSong::recordAgain()
{
    if (auto refused = refusal (activity, Gesture::record))
        return std::unexpected (std::move (*refused));
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
    if (auto refused = refusal (activity, Gesture::setPreRoll))
        return std::unexpected (std::move (*refused));
    if (preRoll == song().preRoll)
        return {};
    auto changed = song();
    changed.preRoll = preRoll;
    package_->changeWithoutHistory (std::move (changed));
    return {};
}

core::Expected<void> OpenSong::setPracticeSpeed (int speedPercent)
{
    if (auto refused = refusal (activity, Gesture::setPracticeSpeed))
        return std::unexpected (std::move (*refused));
    if (auto checked = song::checkPracticeSpeed (speedPercent); ! checked.has_value())
        return std::unexpected (std::move (checked.error()));
    // The same value again tries the renders that failed again.
    changePracticeTarget (speedPercent);
    if (heardLoop().has_value())
        storeActiveLoopPracticeSpeed();
    return {};
}

void OpenSong::changePracticeTarget (int speedPercent)
{
    if (speedPercent != practiceTarget)
        core::logEvent (core::Level::info, "ui.practice_speed_set", { { "from_percent", practiceTarget }, { "to_percent", speedPercent } });
    practiceTarget = speedPercent;
    failedPracticeRenders.clear();
    followPracticeSpeed();
}

void OpenSong::storeActiveLoopPracticeSpeed()
{
    const auto* loop = song::activeLoop (song());
    OVERDUB_CHECK (loop != nullptr, "ui.no_active_loop_for_practice_speed");
    if (loop->practiceSpeedPercent == practiceTarget)
        return;
    const auto loopId = loop->id;
    auto changed = song::setLoopPracticeSpeed (song(), loopId, practiceTarget);
    OVERDUB_CHECK (changed.has_value(), "ui.loop_practice_speed_unset");
    package_->changeWithoutHistory (std::move (*changed));
    core::logEvent (core::Level::info, "ui.loop_practice_speed_set", { { "loop_id", loopId }, { "speed_percent", practiceTarget } });
}

void OpenSong::applyHeardLoopPracticeSpeed()
{
    const auto* loop = heardLoop().has_value() ? song::activeLoop (song()) : nullptr;
    if (loop == nullptr || ! practiceSpeedMayChange())
        return;
    if (loop->practiceSpeedPercent != practiceTarget)
        changePracticeTarget (loop->practiceSpeedPercent);
}

bool OpenSong::practiceSpeedMayChange() const
{
    return ! refusal (activity, Gesture::setPracticeSpeed).has_value();
}

void OpenSong::setExportVideoTake (const song::Id& videoTakeId)
{
    if (song().exportVideoTakeId == videoTakeId || ! std::ranges::contains (song().videoTakes, videoTakeId, &song::VideoTake::id))
        return;
    auto changed = song();
    changed.exportVideoTakeId = videoTakeId;
    package_->changeWithoutHistory (std::move (changed));
}

void OpenSong::setExportFormat (song::ExportFormat format)
{
    if (song().exportFormat == format)
        return;
    auto changed = song();
    changed.exportFormat = format;
    package_->changeWithoutHistory (std::move (changed));
}

void OpenSong::setExportLength (song::VideoLength length)
{
    if (song().exportLength == length)
        return;
    auto changed = song();
    changed.exportLength = length;
    package_->changeWithoutHistory (std::move (changed));
}

void OpenSong::setExportDestination (const std::string& path)
{
    if (song().exportDestination == path)
        return;
    auto changed = song();
    changed.exportDestination = path;
    package_->changeWithoutHistory (std::move (changed));
}

void OpenSong::setTranscribing (bool on)
{
    if (on == transcribing)
        return;
    const auto shiftedBefore = octaveShiftedTrack();
    transcribing = on;
    core::logEvent (core::Level::debug, "ui.transcribe_mix_heard", { { "heard", on } });
    if (binding.engine != nullptr && (octaveShiftedTrack() != shiftedBefore || ! binding.engine->setMix (heardSong())))
        reloadEngine();
    followOctaveShift();
    syncTranscribeListening();
    // Record and Edit play at 100%; Transcribe brings back the looping Loop's speed (ADR 0010).
    if (! on && practiceTarget != song::normalPracticeSpeed && practiceSpeedMayChange())
        changePracticeTarget (song::normalPracticeSpeed);
    applyHeardLoopPracticeSpeed();
    followPracticeSpeed(); // the Loop's window renders first
}

core::Expected<void> OpenSong::setActiveLoop (const std::optional<song::Id>& loopId, bool looping)
{
    if (auto refused = refusal (activity, Gesture::setTranscribeMix))
        return std::unexpected (std::move (*refused));
    auto changed = changeTranscribeMix (song::setActiveLoop (song(), loopId, looping));
    if (! changed.has_value())
        return changed;
    core::logEvent (core::Level::info, "ui.active_loop_set", { { "loop_id", loopId.value_or ("") }, { "looping", song().looping } });
    applyHeardLoopPracticeSpeed();
    followPracticeSpeed(); // the Loop's window renders first
    return changed;
}

core::Expected<void> OpenSong::setLooping (bool looping)
{
    if (auto refused = refusal (activity, Gesture::setTranscribeMix))
        return std::unexpected (std::move (*refused));
    auto changed = changeTranscribeMix (song::setLooping (song(), looping));
    if (! changed.has_value())
        return changed;
    core::logEvent (core::Level::info, "ui.looping_set", { { "looping", looping } });
    applyHeardLoopPracticeSpeed();
    followPracticeSpeed(); // the Loop's window renders first
    return changed;
}

std::optional<engine::LoopRange> OpenSong::heardLoop() const
{
    const auto* loop = song::activeLoop (song());
    if (! transcribing || ! song().looping || loop == nullptr)
        return std::nullopt;
    return engine::LoopRange { loop->start, loop->end };
}

void OpenSong::syncTranscribeListening()
{
    if (binding.engine == nullptr)
        return;
    binding.engine->setLoop (heardLoop());
    binding.engine->setBandFilter (heardBandFilter());
}

core::Expected<void> OpenSong::changeTranscribeMix (core::Expected<song::Song> changed)
{
    if (! changed.has_value())
        return std::unexpected (std::move (changed.error()));
    if (*changed == song())
        return {};
    const auto shiftedBefore = std::pair (octaveShiftedTrack(), song::focusOctaveShift (song()));
    package_->changeWithoutHistory (std::move (*changed));
    const auto shiftChanged = std::pair (octaveShiftedTrack(), song::focusOctaveShift (song())) != shiftedBefore;
    if (binding.engine != nullptr && (shiftChanged || ! binding.engine->setMix (heardSong())))
        reloadEngine();
    syncTranscribeListening();
    followOctaveShift();
    return {};
}

core::Expected<void> OpenSong::setFocusTrack (const std::optional<song::Id>& trackId)
{
    if (auto refused = refusal (activity, Gesture::setTranscribeMix))
        return std::unexpected (std::move (*refused));
    auto changed = changeTranscribeMix (song::setFocusTrack (song(), trackId));
    if (changed.has_value())
        core::logEvent (core::Level::info, "ui.focus_track_set", { { "track_id", trackId.value_or ("") } });
    return changed;
}

core::Expected<void> OpenSong::setOctaveShift (const song::Id& trackId, int octaves)
{
    if (auto refused = refusal (activity, Gesture::setTranscribeMix))
        return std::unexpected (std::move (*refused));
    auto changed = changeTranscribeMix (song::setOctaveShift (song(), trackId, octaves));
    if (changed.has_value())
        core::logEvent (core::Level::info, "ui.octave_shift_set", { { "track_id", trackId }, { "octaves", octaves } });
    return changed;
}

core::Expected<void> OpenSong::setBandFilter (const song::Id& trackId, song::BandFilter filter)
{
    if (auto refused = refusal (activity, Gesture::setTranscribeMix))
        return std::unexpected (std::move (*refused));
    auto changed = changeTranscribeMix (song::setBandFilter (song(), trackId, filter));
    if (changed.has_value())
        core::logEvent (core::Level::info, "ui.band_filter_set", { { "track_id", trackId }, { "on", filter.on }, { "low_hz", filter.lowHz }, { "high_hz", filter.highHz } });
    return changed;
}

std::optional<engine::HeardBandFilter> OpenSong::heardBandFilter() const
{
    const auto* track = song::focusTrack (song());
    if (! transcribing || track == nullptr || ! track->bandFilter.on)
        return std::nullopt;
    return engine::HeardBandFilter { .trackId = track->id, .lowHz = track->bandFilter.lowHz, .highHz = track->bandFilter.highHz };
}

std::optional<song::Id> OpenSong::octaveShiftedTrack() const
{
    const auto* track = song::focusTrack (song());
    if (! transcribing || track == nullptr || track->octaveShift == 0)
        return std::nullopt;
    return track->id;
}

core::Expected<void> OpenSong::setTranscribeMute (const song::Id& trackId, bool mute)
{
    if (auto refused = refusal (activity, Gesture::setTranscribeMix))
        return std::unexpected (std::move (*refused));
    auto changed = changeTranscribeMix (song::setTranscribeMute (song(), trackId, mute));
    if (changed.has_value())
        core::logEvent (core::Level::debug, "ui.transcribe_mute_set", { { "track_id", trackId }, { "mute", mute } });
    return changed;
}

core::Expected<void> OpenSong::setTranscribeSolo (const song::Id& trackId, bool solo)
{
    if (auto refused = refusal (activity, Gesture::setTranscribeMix))
        return std::unexpected (std::move (*refused));
    auto changed = changeTranscribeMix (song::setTranscribeSolo (song(), trackId, solo));
    if (changed.has_value())
        core::logEvent (core::Level::debug, "ui.transcribe_solo_set", { { "track_id", trackId }, { "solo", solo } });
    return changed;
}

void OpenSong::stop()
{
    if (binding.engine == nullptr)
        return;
    if (auto* pass = std::get_if<PassActivity> (&activity))
        pass->recorder->requestStop();
    binding.engine->stop();
}

core::Expected<void> OpenSong::playOrStop()
{
    if (auto refused = refusal (activity, Gesture::playOrStop))
        return std::unexpected (std::move (*refused));
    if (binding.engine == nullptr)
        return {};
    recordAgainAfterPass = false; // Space after Record Again stops for good
    if (isRecording() || binding.engine->isPlaying())
    {
        stop();
        return {};
    }
    if (const auto loop = heardLoop(); loop.has_value())
    {
        const auto position = binding.engine->position();
        if (position < loop->start || position >= loop->end)
            binding.engine->seek (loop->start);
    }
    binding.engine->play();
    return {};
}

void OpenSong::rewind()
{
    if (binding.engine == nullptr)
        return;
    if (auto* pass = std::get_if<PassActivity> (&activity))
        pass->recorder->requestStop();
    binding.engine->seek (0);
}

core::Expected<void> OpenSong::seek (song::SampleCount sample)
{
    if (auto refused = refusal (activity, Gesture::seek))
        return std::unexpected (std::move (*refused));
    const auto position = std::max<song::SampleCount> (0, sample);
    if (binding.engine != nullptr)
    {
        binding.engine->seek (position);
        return {};
    }
    auto changed = song();
    changed.playhead = position;
    package_->changeWithoutHistory (std::move (changed));
    return {};
}

core::Expected<void> OpenSong::edit (song::Song changed, std::string label)
{
    if (auto refused = refusal (activity, Gesture::edit))
        return std::unexpected (std::move (*refused));
    const auto before = song();
    auto result = package_->edit (std::move (changed), std::move (label));
    updateEngine (before);
    return result;
}

core::Expected<song::Id> OpenSong::addInsert (const song::Id& trackId, const plugin::AvailableAudioUnit& plugin)
{
    if (auto refused = refusal (activity, Gesture::addInsert))
        return std::unexpected (std::move (*refused));
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

core::Expected<void> OpenSong::previewMix (const song::Song& candidate)
{
    if (auto refused = refusal (activity, Gesture::previewMix))
        return std::unexpected (std::move (*refused));
    if (binding.engine != nullptr && song::differsOnlyInMix (song(), candidate))
        binding.engine->setMix (withHeardMix (candidate));
    return {};
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
    if (auto refused = refusal (activity, Gesture::undo))
        return std::unexpected (std::move (*refused));
    const auto before = song();
    auto result = package_->undo();
    updateEngine (before);
    return result;
}

core::Expected<void> OpenSong::redo()
{
    if (auto refused = refusal (activity, Gesture::redo))
        return std::unexpected (std::move (*refused));
    const auto before = song();
    auto result = package_->redo();
    updateEngine (before);
    return result;
}

core::Expected<void> OpenSong::setArmed (const song::Id& trackId, bool armed)
{
    if (auto refused = refusal (activity, Gesture::setArmed))
        return std::unexpected (std::move (*refused));
    package_->changeWithoutHistory (withArmed (song(), trackId, armed));
    reloadEngine();
    return {};
}

core::Expected<void> OpenSong::setInputChannels (const song::Id& trackId, std::vector<int> inputChannels)
{
    if (auto refused = refusal (activity, Gesture::setInputChannels))
        return std::unexpected (std::move (*refused));
    package_->changeWithoutHistory (withInputChannels (song(), trackId, std::move (inputChannels)));
    reloadEngine();
    return {};
}

core::Expected<void> OpenSong::setMonitoring (const song::Id& trackId, bool on)
{
    if (auto refused = refusal (activity, Gesture::setMonitoring))
        return std::unexpected (std::move (*refused));
    if (on)
        monitored.insert (trackId);
    else
        monitored.erase (trackId);
    reloadEngine();
    return {};
}

std::vector<core::Expected<song::Id>> OpenSong::importFiles (const std::vector<juce::File>& files)
{
    if (auto refused = refusal (activity, Gesture::changeTracks))
        return std::vector<core::Expected<song::Id>> (files.size(), std::unexpected (*refused));
    auto results = package_->importBackingTracks (files);
    reloadEngine();
    followTranspose();
    followPracticeSpeed();
    return results;
}

core::Expected<void> OpenSong::startSeparation (const song::Id& trackId, song::SongPackage::Clock::time_point now)
{
    if (isSeparating())
        return {};
    if (auto refused = refusal (activity, Gesture::startSeparation))
        return std::unexpected (std::move (*refused));
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
    followTranspose();
    followPracticeSpeed();
    return std::nullopt;
}

core::Expected<void> OpenSong::startSampleRateConversion (int toRate, song::SongPackage::Clock::time_point now)
{
    if (isConverting())
        return {};
    if (auto refused = refusal (activity, Gesture::startSampleRateConversion))
        return std::unexpected (std::move (*refused));
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
    const auto before = song();
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
    if (auto error = advanceTranspose (now))
        errors.push_back (std::move (*error));
    if (auto error = advancePracticeSpeed (now))
        errors.push_back (std::move (*error));
    if (auto error = advanceOctaveShift (now))
        errors.push_back (std::move (*error));
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
        if (position != song().playhead)
        {
            auto changed = song();
            changed.playhead = position;
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
    if (! song().cameraArmed || ! open.has_value() || (pass != nullptr && pass->cameraLost) || std::ranges::contains (cameras, open->id, &capture::CameraDevice::id))
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
    if (! song().cameraArmed)
        return;
    // The choice stays: the camera is offered again when it comes back, and arming reopens it.
    auto changed = song();
    changed.cameraArmed = false;
    package_->changeWithoutHistory (std::move (changed));
}

core::Expected<void> OpenSong::setCameraArmed (bool armed)
{
    if (auto refused = refusal (activity, Gesture::setCameraArmed))
        return std::unexpected (std::move (*refused));
    if (camera == nullptr || armed == song().cameraArmed)
        return {};
    auto changed = song();
    changed.cameraArmed = armed;
    package_->changeWithoutHistory (std::move (changed));
    cameras = camera->devices();
    return syncCamera();
}

core::Expected<void> OpenSong::setCameraDevice (const std::string& cameraId)
{
    if (auto refused = refusal (activity, Gesture::setCameraDevice))
        return std::unexpected (std::move (*refused));
    if (cameraId == song().cameraDeviceId)
        return {};
    auto changed = song();
    changed.cameraDeviceId = cameraId;
    package_->changeWithoutHistory (std::move (changed));
    if (camera == nullptr)
        return {};
    cameras = camera->devices();
    if (song().cameraArmed)
        camera->close(); // reopened on the new choice
    return syncCamera();
}

void* OpenSong::cameraPreviewLayer() const
{
    if (camera == nullptr || ! song().cameraArmed || ! camera->openCamera().has_value())
        return nullptr;
    return camera->previewLayer();
}

core::Expected<void> OpenSong::syncCamera()
{
    if (camera == nullptr)
        return {};
    if (! song().cameraArmed)
    {
        camera->close();
        return {};
    }
    if (const auto open = camera->openCamera(); open.has_value() && open->id == song().cameraDeviceId)
        return {};

    auto opened = camera->open (song().cameraDeviceId);
    auto changed = song();
    if (! opened.has_value())
    {
        changed.cameraArmed = false;
        package_->changeWithoutHistory (std::move (changed));
        return std::unexpected (opened.error());
    }
    // The default camera opened for no (or a missing) choice becomes the choice.
    if (! song().cameraDeviceId.empty() && opened->id != song().cameraDeviceId)
        core::logEvent (core::Level::warn, "ui.camera_fallback", { { "chosen_camera_id", song().cameraDeviceId }, { "opened_camera_id", opened->id } });
    changed.cameraDeviceId = opened->id;
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

void OpenSong::updateEngine (const song::Song& before)
{
    if (before.sampleRate != song().sampleRate)
    {
        // The device runs at the old rate: the owner opens it again at the new one.
        core::logEvent (core::Level::info, "ui.song_rate_changed", { { "from_rate", before.sampleRate }, { "to_rate", song().sampleRate } });
        for (const auto& error : detachAudio())
            core::logError (error);
        sampleRateChanged = true;
        return;
    }
    if (binding.engine != nullptr && song::differsOnlyInMix (before, song()) && binding.engine->setMix (heardSong()))
    {
        syncTranscribeListening();
        return;
    }
    reloadEngine();
    followTranspose();
    followPracticeSpeed();
    followOctaveShift();
}

void OpenSong::reloadEngine()
{
    if (auto* exporting = std::get_if<ExportActivity> (&activity))
    {
        exporting->run->followSong();
        return;
    }
    inserts.sync (song());
    if (binding.engine != nullptr)
        binding.engine->loadSong (heardSong(), package_->location(), inserts.insertFactory(), monitored, heardPracticePlayback());
    syncTranscribeListening();
}

song::Song OpenSong::heardSong() const
{
    auto heard = withHeardMix (song());
    heard.transpose = heardShift;
    return heard;
}

song::Song OpenSong::withHeardMix (song::Song candidate) const
{
    return transcribing ? song::withTranscribeMix (std::move (candidate)) : candidate;
}

std::optional<core::Error> OpenSong::advanceTranspose (song::SongPackage::Clock::time_point now)
{
    const auto target = song().transpose;
    if (target != failedShift)
        failedRenders.clear();
    std::optional<core::Error> failure;
    if (transposeRun != nullptr)
    {
        if (transposeRun->transpose() != target || rendersRemovedBackingTrack (*transposeRun, song()))
        {
            transposeRun->cancel (now);
            transposeRun.reset();
            sweptBackingTracks.reset();
        }
        else if (auto ended = transposeRun->advance (now))
        {
            transposeRun.reset();
            sweptBackingTracks.reset(); // it may have rendered a Backing Track an undo removed since
            if (! ended->has_value())
            {
                failure = ended->error();
                failedShift = target;
                failedRenders.insert (failure->fields.value ("backing_track_id", std::string()));
            }
            else if (heardShift == target)
                reloadEngine(); // Backing Tracks added since: their renders play now
        }
        else
            return std::nullopt;
    }

    auto backingTrackIds = song().backingTracks | std::views::transform (&song::BackingTrack::id) | std::ranges::to<std::vector>();
    if (backingTrackIds != sweptBackingTracks)
    {
        if (const auto deleted = song::deleteTransposeRendersOfRemovedBackingTracks (package_->location(), song()); deleted > 0)
            core::logEvent (core::Level::info, "ui.transpose_renders_deleted", { { "count", deleted } });
        sweptBackingTracks = std::move (backingTrackIds);
    }

    auto missing = song::missingTransposeRenders (package_->location(), song());
    std::erase_if (missing, [this] (const song::TransposeRenderJob& job) { return failedRenders.contains (job.backingTrackId); });
    if (! missing.empty())
    {
        transposeRun = song::TransposeRun::start (song().sampleRate, target, std::move (missing), now);
        return failure;
    }
    if (heardShift != target && failedRenders.empty())
    {
        core::logEvent (core::Level::info, "ui.transpose_heard", { { "from_semitones", heardShift.semitones }, { "from_cents", heardShift.cents }, { "to_semitones", target.semitones }, { "to_cents", target.cents } });
        heardShift = target;
        song::deleteTransposeRendersExcept (package_->location(), target);
        reloadEngine();
    }
    return failure;
}

engine::PracticePlayback OpenSong::heardPracticePlayback() const
{
    return {
        .speedPercent = heardPractice.speedPercent,
        .renders = song::practiceRendersOf (practiceFolder, song(), heardPractice, practiceLoopWindow()),
        .octaveShifted = heardOctaveShift(),
    };
}

song::PracticeRenderKey OpenSong::octaveKey() const
{
    // At 100% the Backing Tracks play at the heard Transpose; at another speed, at the renders'.
    const auto transpose = heardPractice.speedPercent == song::normalPracticeSpeed ? heardShift : heardPractice.transpose;
    return { .speedPercent = heardPractice.speedPercent, .transpose = transpose, .octaveShift = song::focusOctaveShift (song()) };
}

song::Song OpenSong::octaveShiftedSong (const song::Id& trackId) const
{
    auto shifted = song();
    std::erase_if (shifted.tracks, [&trackId] (const song::Track& track) { return track.id != trackId; });
    return shifted;
}

std::optional<engine::OctaveShiftedTrack> OpenSong::heardOctaveShift() const
{
    const auto trackId = octaveShiftedTrack();
    if (! trackId.has_value())
        return std::nullopt;
    return engine::OctaveShiftedTrack { .trackId = *trackId, .renders = song::practiceRendersOf (octaveRenderFolder(), octaveShiftedSong (*trackId), octaveKey()) };
}

bool OpenSong::octaveRunIsWanted() const
{
    return octaveShiftedTrack() == octaveRunTrackId && octaveRun->key() == octaveKey();
}

std::optional<core::Error> OpenSong::advanceOctaveShift (song::SongPackage::Clock::time_point now)
{
    const auto trackId = octaveShiftedTrack();
    const auto key = trackId.has_value() ? octaveKey() : song::PracticeRenderKey {};
    if (trackId != failedOctaveTrackId || key != failedOctaveKey)
        failedOctaveRenders.clear();
    std::optional<core::Error> failure;
    if (octaveRun != nullptr)
    {
        if (! octaveRunIsWanted())
        {
            octaveRun->cancel (now);
            octaveRun.reset();
        }
        else if (auto ended = octaveRun->advance (now))
        {
            const auto failedSource = octaveRun->failedSource();
            octaveRun.reset();
            if (! ended->has_value())
            {
                failure = ended->error();
                failedOctaveTrackId = trackId;
                failedOctaveKey = key;
                failedOctaveRenders.insert (*failedSource);
                reloadEngine(); // the renders made before the failed one play
            }
            else
            {
                reloadEngine(); // the renders made play now
                if (const auto deleted = song::deletePracticeRendersExcept (octaveRenderFolder(), octaveShiftedSong (*trackId), key); deleted > 0)
                    core::logEvent (core::Level::info, "ui.octave_renders_deleted", { { "count", deleted } });
            }
        }
        else
            return std::nullopt;
    }
    if (! trackId.has_value())
        return failure;

    auto missing = song::missingPracticeRenders (package_->location(), octaveRenderFolder(), octaveShiftedSong (*trackId), key);
    std::erase_if (missing, [this] (const song::PracticeRenderJob& job) { return failedOctaveRenders.contains (job.source); });
    if (! missing.empty())
    {
        // A folder that cannot be made fails the render's write, handed back by the run.
        octaveRenderFolder().createDirectory();
        octaveRun = song::PracticeSpeedRun::start (song().sampleRate, key, std::nullopt, std::move (missing), now);
        octaveRunTrackId = *trackId;
    }
    return failure;
}

void OpenSong::followOctaveShift()
{
    if (octaveRun != nullptr && octaveRunIsWanted())
        return;
    // Only a run that ends fails: here none runs, or advanceOctaveShift cancels it.
    const auto failure = advanceOctaveShift (song::SongPackage::Clock::now());
    OVERDUB_CHECK (! failure.has_value(), "ui.octave_shift_failed_without_run");
}

std::optional<song::SampleRange> OpenSong::practiceLoopWindow() const
{
    const auto loop = heardLoop();
    if (! loop.has_value() || practiceTarget == song::normalPracticeSpeed)
        return std::nullopt;
    return song::loopRenderWindow (loop->start, loop->end, song().sampleRate);
}

bool OpenSong::practiceRunIsWanted() const
{
    return practiceRun->key() == practiceKey() && practiceRun->loopWindow() == practiceLoopWindow();
}

void OpenSong::deleteUnheardPracticeRenders()
{
    if (const auto deleted = song::deletePracticeRendersExcept (practiceFolder, song(), heardPractice, practiceLoopWindow()); deleted > 0)
        core::logEvent (core::Level::info, "ui.practice_renders_deleted", { { "count", deleted } });
}

song::PracticeRenderKey OpenSong::practiceKey() const
{
    if (practiceTarget == song::normalPracticeSpeed)
        return {};
    return { .speedPercent = practiceTarget, .transpose = song().transpose };
}

std::optional<core::Error> OpenSong::advancePracticeSpeed (song::SongPackage::Clock::time_point now)
{
    const auto target = practiceKey();
    const auto window = practiceLoopWindow();
    if (target != failedPracticeKey)
        failedPracticeRenders.clear();
    std::optional<core::Error> failure;
    if (practiceRun != nullptr)
    {
        if (! practiceRunIsWanted())
        {
            practiceRun->cancel (now);
            practiceRun.reset();
        }
        else if (auto ended = practiceRun->advance (now))
        {
            const auto failedSource = practiceRun->failedSource();
            practiceRun.reset();
            if (! ended->has_value())
            {
                failure = ended->error();
                failedPracticeKey = target;
                failedPracticeRenders.insert (*failedSource);
            }
            else if (heardPractice == target)
            {
                reloadEngine(); // sources that had no render, or only a Loop's, play theirs now
                deleteUnheardPracticeRenders();
            }
        }
        else
            return std::nullopt;
    }

    const auto missingOf = [&] (std::optional<song::SampleRange> rendersWindow) {
        auto missing = song::missingPracticeRenders (package_->location(), practiceFolder, song(), target, rendersWindow);
        std::erase_if (missing, [this] (const song::PracticeRenderJob& job) { return failedPracticeRenders.contains (job.source); });
        return missing;
    };
    const auto startRun = [&] (std::vector<song::PracticeRenderJob> jobs) {
        // A folder that cannot be made fails the render's write, handed back by the run.
        practiceFolder.createDirectory();
        practiceRun = song::PracticeSpeedRun::start (song().sampleRate, target, window, std::move (jobs), now);
    };
    // While a Loop loops, its range renders first (issue #231); the whole Song's after.
    if (window.has_value())
        if (auto missingInLoop = missingOf (window); ! missingInLoop.empty())
        {
            startRun (std::move (missingInLoop));
            return failure;
        }
    auto missing = missingOf (std::nullopt);
    // While a Loop loops it is heard once its renders are there; otherwise once every render is.
    if (heardPractice != target && failedPracticeRenders.empty() && (missing.empty() || window.has_value()))
    {
        core::logEvent (core::Level::info,
                        "ui.practice_speed_heard",
                        { { "from_percent", heardPractice.speedPercent }, { "to_percent", target.speedPercent }, { "semitones", target.transpose.semitones }, { "cents", target.transpose.cents } });
        heardPractice = target;
        reloadEngine();
        deleteUnheardPracticeRenders();
    }
    if (! missing.empty())
        startRun (std::move (missing));
    return failure;
}

void OpenSong::followPracticeSpeed()
{
    if (practiceRun != nullptr && practiceRunIsWanted())
        return;
    // Only a run that ends fails: here none runs, or advancePracticeSpeed cancels it.
    const auto failure = advancePracticeSpeed (song::SongPackage::Clock::now());
    OVERDUB_CHECK (! failure.has_value(), "ui.practice_speed_failed_without_run");
}

void OpenSong::followTranspose()
{
    if (transposeRun != nullptr && ! rendersRemovedBackingTrack (*transposeRun, song()))
        return;
    // Only a run that ends fails: here none runs, or advanceTranspose cancels it.
    const auto failure = advanceTranspose (song::SongPackage::Clock::now());
    OVERDUB_CHECK (! failure.has_value(), "ui.transpose_failed_without_run");
}

core::Expected<void> OpenSong::startExport (exporting::ExportRequest request, song::SongPackage::Clock::time_point now)
{
    if (isExporting())
        return {};
    if (auto refused = refusal (activity, Gesture::startExport))
        return std::unexpected (std::move (*refused));
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
