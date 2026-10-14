#include "ui/OpenSong.h"

#include "capture/CameraCalibration.h"
#include "core/Log.h"
#include "song/InsertEdits.h"
#include "song/MixEdits.h"
#include "ui/TrackRowEdits.h"

#include <algorithm>

namespace overdub::ui
{
namespace
{
// Plugins are prepared for this block size when no device is open.
constexpr int blockSizeWithoutDevice = 512;
} // namespace

OpenSong::OpenSong (std::unique_ptr<song::SongPackage> package, AudioBinding audio, plugin::Instantiator instantiate)
    : package_ (std::move (package)),
      inserts (std::move (instantiate),
               [this] (const std::string& hash) { return package_->loadPluginState (hash); },
               package_->song().sampleRate,
               blockSizeOf (audio)),
      playback (package_->location())
{
    package_->setPluginStateCapture ([this] (song::Song& song) { capturePluginState (song); });
    inserts.sync (song());
    attachAudio (std::move (audio));
}

OpenSong::~OpenSong()
{
    for (const auto& error : detachAudio())
        core::logError (error);
    // Closing the Song waits for a movie still completing (ADR 0003, issue #90 amendment).
    if (recorder != nullptr)
        for (const auto& error : finishPass (recorder->finish()))
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
                                     { { "song_rate", song().sampleRate }, { "device_rate", audio.sampleRate } } };
        audio.engine = nullptr;
    }
    binding = std::move (audio);
    if (binding.engine == nullptr)
        return;
    // Invariant (see attachAudio in OpenSong.h): no engine renders the plugins now.
    inserts.prepare (song().sampleRate, blockSizeOf (binding));
    binding.engine->stop();
    binding.engine->setMonitoring (true); // which Tracks are heard is decided by songForEngine
    reloadEngine();
    binding.engine->seek (song().playhead);
}

std::vector<core::Error> OpenSong::detachAudio()
{
    // The export renders the plugins the next engine is prepared for: it must stop first.
    if (exportRun.has_value())
    {
        exportRun->stop ("audio_detached");
        exportRun.reset();
    }
    std::vector<core::Error> errors;
    if (recorder != nullptr && binding.engine != nullptr)
    {
        binding.engine->stop();
        if (auto finished = recorder->release()) // a movie still completing: poll() finishes the Pass
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
    return recorder != nullptr && recorder->phase() == capture::PassPhase::saving;
}

core::Expected<void> OpenSong::record()
{
    if (recorder != nullptr)
        return {};
    if (isExporting())
        return std::unexpected (core::Error { "ui.export_running", "an export is running", {} });
    if (binding.engine == nullptr)
        return std::unexpected (*audioProblem());

    const auto passNumber = nextPassNumber (song());
    const auto preRollSamples = (song::SampleCount) song::preRollSeconds (song().preRoll) * song().sampleRate;
    const auto usePreRoll = ! binding.engine->isPlaying() && preRollSamples > 0;
    const auto recordPoint = binding.engine->position();

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

    recorder = std::move (*started);
    recordingPass = passNumber;
    preRollPoint = options.recordPoint;
    // A Mixer preview still applied (a drag the Pass interrupts) gives way to the Song's mix.
    binding.engine->setMix (song());
    if (usePreRoll)
    {
        const auto playFrom = std::max<song::SampleCount> (0, recordPoint - preRollSamples);
        binding.engine->seek (playFrom);
        core::logEvent (core::Level::info, "ui.pre_roll_started", { { "record_point", recordPoint }, { "play_from", playFrom }, { "pre_roll_seconds", song::preRollSeconds (song().preRoll) } });
    }
    if (! binding.engine->isPlaying())
        binding.engine->play();
    return {};
}

std::optional<song::SampleCount> OpenSong::preRollRecordPoint() const
{
    if (recorder == nullptr || ! preRollPoint.has_value() || recorder->phase() != capture::PassPhase::waiting)
        return std::nullopt;
    return preRollPoint;
}

std::optional<song::SampleCount> OpenSong::runningPassStart() const
{
    return recorder != nullptr ? recorder->runningPassStart() : std::nullopt;
}

void OpenSong::setPreRoll (song::PreRoll preRoll)
{
    if (recorder != nullptr || preRoll == song().preRoll)
        return;
    auto changed = song();
    changed.preRoll = preRoll;
    package_->changeWithoutHistory (std::move (changed));
}

void OpenSong::setExportVideoTake (const song::Id& videoTakeId)
{
    if (song().exportVideoTakeId == videoTakeId || ! std::ranges::contains (song().videoTakes, videoTakeId, &song::VideoTake::id))
        return;
    auto changed = song();
    changed.exportVideoTakeId = videoTakeId;
    package_->changeWithoutHistory (std::move (changed));
}

void OpenSong::stop()
{
    if (binding.engine == nullptr)
        return;
    if (recorder != nullptr)
        recorder->requestStop();
    binding.engine->stop();
}

void OpenSong::playOrStop()
{
    if (binding.engine == nullptr || isExporting())
        return;
    if (recorder != nullptr || binding.engine->isPlaying())
        stop();
    else
        binding.engine->play();
}

void OpenSong::rewind()
{
    if (binding.engine == nullptr)
        return;
    if (recorder != nullptr)
        recorder->requestStop();
    binding.engine->seek (0);
}

void OpenSong::seek (song::SampleCount sample)
{
    if (recorder != nullptr)
        return;
    const auto position = std::max<song::SampleCount> (0, sample);
    if (binding.engine != nullptr)
    {
        binding.engine->seek (position);
        return;
    }
    auto changed = song();
    changed.playhead = position;
    package_->changeWithoutHistory (std::move (changed));
}

core::Expected<void> OpenSong::edit (song::Song changed, std::string label)
{
    if (recorder != nullptr)
        return {};
    const auto before = song();
    auto result = package_->edit (std::move (changed), std::move (label));
    updateEngine (before);
    return result;
}

core::Expected<song::Id> OpenSong::addInsert (const song::Id& trackId, const plugin::AvailableAudioUnit& plugin)
{
    if (recorder != nullptr)
        return song::Id {};
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
    if (recorder != nullptr || binding.engine == nullptr || ! song::differsOnlyInMix (song(), candidate))
        return;
    binding.engine->setMix (candidate);
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
    if (recorder != nullptr)
        return {};
    const auto before = song();
    auto result = package_->undo();
    updateEngine (before);
    return result;
}

core::Expected<void> OpenSong::redo()
{
    if (recorder != nullptr)
        return {};
    const auto before = song();
    auto result = package_->redo();
    updateEngine (before);
    return result;
}

void OpenSong::setArmed (const song::Id& trackId, bool armed)
{
    if (recorder != nullptr)
        return;
    package_->changeWithoutHistory (withArmed (song(), trackId, armed));
    reloadEngine();
}

void OpenSong::setInputChannels (const song::Id& trackId, std::vector<int> inputChannels)
{
    if (recorder != nullptr)
        return;
    package_->changeWithoutHistory (withInputChannels (song(), trackId, std::move (inputChannels)));
    reloadEngine();
}

void OpenSong::setMonitoring (const song::Id& trackId, bool on)
{
    if (recorder != nullptr)
        return;
    if (on)
        monitored.insert (trackId);
    else
        monitored.erase (trackId);
    reloadEngine();
}

std::vector<core::Expected<song::Id>> OpenSong::importFiles (const std::vector<juce::File>& files)
{
    if (recorder != nullptr)
        return {};
    auto results = package_->importBackingTracks (files);
    reloadEngine();
    return results;
}

std::vector<core::Error> OpenSong::poll (song::SongPackage::Clock::time_point now)
{
    auto errors = handleCrashes();
    for (auto& error : handleCameraChanges())
        errors.push_back (std::move (error));
    if (exportRun.has_value())
        if (auto error = endExportIfEnded (exportRun->advance (now, audioLetGoOfPlugins())))
            errors.push_back (std::move (*error));
    if (recorder != nullptr)
        if (auto finished = recorder->poll (now))
            for (auto& error : finishPass (std::move (*finished)))
                errors.push_back (std::move (error));

    // A crashed plugin reloads only once no Pass or export runs (a Pass keeps its plan anyway).
    if (recorder == nullptr && ! isExporting())
        inserts.reloadCrashed (song());
    // During a Pass the plan stays as it started; finishPass rebuilds it.
    if (recorder == nullptr && inserts.takeGraphChanged())
        reloadEngine();
    if (inserts.takeStateChanged())
        package_->changeWithoutHistory (song(), now);

    if (recorder == nullptr && binding.engine != nullptr && ! binding.engine->isPlaying())
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
    if (exportRun.has_value())
        if (auto error = endExportIfEnded (exportRun->crashed (crashes)))
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
    const auto passNumber = recordingPass;
    recorder.reset();
    recordingPass = 0;
    preRollPoint.reset();
    if (cameraLostDuringPass)
        disarmLostCamera();
    reloadEngine(); // the Pass's Takes and Clips, when it was added

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
        return errors;
    }
    for (auto& notice : finished.pass->notices) // the Pass stands; its Video Take may not
        errors.push_back (std::move (notice));
    return errors;
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

    const auto open = camera->openCamera();
    if (! song().cameraArmed || ! open.has_value() || cameraLostDuringPass || std::ranges::contains (cameras, open->id, &capture::CameraDevice::id))
        return {};

    // The Armed camera is gone. A running Pass keeps its movie (written until the camera went) and
    // records on; the camera is disarmed once the Pass is finished.
    const auto duringPass = recorder != nullptr;
    core::Error lost { "ui.camera_disconnected", "the Armed camera was disconnected", { { "camera_id", open->id }, { "during_pass", duringPass } } };
    if (duringPass)
        cameraLostDuringPass = true;
    else
        disarmLostCamera();
    return { lost };
}

void OpenSong::disarmLostCamera()
{
    cameraLostDuringPass = false;
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
    if (recorder != nullptr || camera == nullptr || armed == song().cameraArmed)
        return {};
    auto changed = song();
    changed.cameraArmed = armed;
    package_->changeWithoutHistory (std::move (changed));
    cameras = camera->devices();
    return syncCamera();
}

core::Expected<void> OpenSong::setCameraDevice (const std::string& cameraId)
{
    if (recorder != nullptr || cameraId == song().cameraDeviceId)
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
    if (binding.engine != nullptr && song::differsOnlyInMix (before, song()) && binding.engine->setMix (song()))
        return;
    reloadEngine();
}

void OpenSong::reloadEngine()
{
    if (exportRun.has_value())
    {
        exportRun->followSong();
        return;
    }
    inserts.sync (song());
    if (binding.engine != nullptr)
        binding.engine->loadSong (songForEngine (song(), monitored), package_->location(), inserts.insertFactory());
}

core::Expected<void> OpenSong::startExport (exporting::ExportRequest request, song::SongPackage::Clock::time_point now)
{
    if (recorder != nullptr)
        return std::unexpected (core::Error { "ui.stop_recording_first", "a Pass is running", {} });
    if (isExporting())
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
    exportRun.emplace (std::move (request), std::move (movie), *package_, inserts, blockSizeOf (binding), now);
    // Without audio nothing else renders the plugins: the export starts now.
    if (auto error = endExportIfEnded (exportRun->advance (now, audioLetGoOfPlugins())))
        return std::unexpected (std::move (*error));
    return {};
}

bool OpenSong::audioLetGoOfPlugins() const
{
    return binding.engine == nullptr || ! binding.engine->hasPendingPlan();
}

std::optional<core::Error> OpenSong::endExportIfEnded (std::optional<ExportEnd> end)
{
    if (! end.has_value())
        return std::nullopt;
    exportRun.reset();
    reloadEngine();
    lastExport = std::move (end->file);
    return std::move (end->error);
}
} // namespace overdub::ui
