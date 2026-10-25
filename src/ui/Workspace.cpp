#include "ui/Workspace.h"

#include "capture/ClapSelftest.h"
#include "core/Log.h"
#include "engine/LatencyCalibration.h"
#include "ui/SongFiles.h"
#include "ui/UserMessages.h"

namespace overdub::ui
{
WorkspaceFiles WorkspaceFiles::defaults()
{
    return WorkspaceFiles {
        .deviceSettings = engine::defaultDeviceSettingsFile(),
        .latencyCalibration = engine::defaultLatencyCalibrationFile(),
        .cameraCalibration = capture::defaultCameraCalibrationFile(),
        .separationModel = song::defaultSeparationModelFile(),
        .separationHelper = song::defaultSeparationHelperFile(),
    };
}

Workspace::Workspace (MainComponent& mainContent,
                      std::unique_ptr<LiveAudio> audio,
                      OpenLiveAudio openLiveAudio,
                      capture::Camera* songCamera,
                      plugin::Instantiator instantiator,
                      WorkspaceFiles workspaceFiles)
    : content (mainContent),
      openAudio (std::move (openLiveAudio)),
      camera (songCamera),
      instantiate (std::move (instantiator)),
      files (std::move (workspaceFiles)),
      live (std::move (audio))
{
    watchDevices();
    if (live->engine() == nullptr)
        content.showNotice ("ui.no_audio_device");
    showLatencyNoticeIfUncalibrated();
}

Workspace::~Workspace()
{
    onOpenSongChanged = nullptr; // the owner is being torn down too
    latencyRun.reset();
    closeSong();
}

bool Workspace::isIdle() const
{
    return ! latencyMeasuring() && ! (song != nullptr && (song->isRecording() || song->isSeparating() || song->isConverting()));
}

bool Workspace::idleOrNotice()
{
    if (isIdle())
        return true;
    if (latencyMeasuring())
        content.showNotice ("ui.calibration_running");
    else if (song->isSeparating())
        content.showNotice ("ui.separation_running");
    else if (song->isConverting())
        content.showNotice ("ui.conversion_running");
    else
        content.showNotice ("ui.stop_recording_first");
    return false;
}

bool Workspace::mayQuitOrNotice()
{
    if (song == nullptr || ! song->isSeparating())
        return true;
    content.showNotice ("ui.separation_running");
    return false;
}

//==============================================================================
// Songs

void Workspace::showSong (std::unique_ptr<song::SongPackage> package, const std::vector<song::Notice>& notices)
{
    closeSong();
    // The Song's rate is fixed (ADR 0004): a device that offers it is switched to it.
    const auto songRate = package->song().sampleRate;
    const auto rateSwitch = engine::sampleRateForSong (live->sampleRate(), songRate, live->deviceChoices().sampleRates).has_value();
    if (rateSwitch)
    {
        live.reset();
        live = openAudio (songRate);
        watchDevices();
        if (devicePanel != nullptr)
            devicePanel->show (live->deviceChoices());
    }
    song = std::make_unique<OpenSong> (std::move (package), live->audioBinding(), instantiate);
    if (files.separationHelper != juce::File())
        song->setSeparationTools (song::SeparationTools { .model = files.separationModel, .startHelper = song::separationHelperProcess (files.separationHelper) });
    showRateMismatch();
    if (rateSwitch)
        showLatencyNoticeIfUncalibrated();
    // A Song saved with its camera Armed opens it now; without camera access that is a Notice.
    if (auto attached = song->attachCamera (camera, files.cameraCalibration); ! attached.has_value())
        content.handleError (attached.error());
    content.setOpenSong (song.get());
    for (const auto& notice : notices)
        content.showNotice (notice.code);
    refreshCalibrationPanel();
    if (onOpenSongChanged)
        onOpenSongChanged();
}

void Workspace::openSongFrom (const juce::File& chosen)
{
    // A package is never open twice: opening the open Song again changes nothing.
    if (song != nullptr && song->package().location() == packageLocationOpened (chosen))
        return;
    auto opened = openSongAt (chosen);
    if (! opened.has_value())
        return content.handleError (opened.error());
    showSong (std::move (opened->package), opened->notices);
}

void Workspace::closeSong()
{
    detachAudio();
    content.setOpenSong (nullptr);
    song.reset();
    if (onOpenSongChanged)
        onOpenSongChanged();
}

void Workspace::detachAudio()
{
    if (song == nullptr)
        return;
    for (const auto& error : song->detachAudio())
        content.handleError (error);
}

std::optional<int> Workspace::openSongRate() const
{
    return song != nullptr ? std::optional<int> (song->song().sampleRate) : std::nullopt;
}

void Workspace::showRateMismatch()
{
    // ui.no_audio_device has its own Notice; only the rate is said here.
    if (const auto problem = song->audioProblem(); problem.has_value() && problem->code == "ui.sample_rate_mismatch")
        content.handleError (*problem);
}

//==============================================================================
// Settings and device changes

void Workspace::attachSettingsPanels (DeviceSettingsPanel* devices, CalibrationPanel* calibration)
{
    devicePanel = devices;
    calibrationPanel = calibration;
    if (devicePanel != nullptr)
    {
        devicePanel->onApply = [this] (const engine::DeviceSettings& settings) { applyDeviceSettings (settings); };
        devicePanel->offerForDevices = [this] (const std::string& input, const std::string& output) { return live->offerFor (input, output); };
    }
    if (calibrationPanel != nullptr)
    {
        calibrationPanel->onMeasureLatency = [this] { measureLatency(); };
        calibrationPanel->onSaveLatency = [this] (song::SampleCount offset) { saveLatencyCalibration (offset); };
        calibrationPanel->onMeasureClap = [this] { measureClapPass(); };
        calibrationPanel->onSaveCamera = [this] (const capture::CameraCalibrationKey& key, double offsetMs) { saveCameraCalibration (key, offsetMs); };
    }
}

void Workspace::refreshSettingsPanels()
{
    if (devicePanel != nullptr)
        devicePanel->show (live->deviceChoices());
    refreshCalibrationPanel();
}

void Workspace::applyDeviceSettings (const engine::DeviceSettings& settings)
{
    if (! idleOrNotice())
        return;
    core::logEvent (core::Level::info, "ui.device_choice_changed", engine::deviceSettingsFields (settings));

    // The engine's shape is fixed per device (engine/Engine.h), so a new choice opens a new
    // LiveAudio, which opens the saved choice (or falls back to the default and logs why).
    if (auto written = engine::writeDeviceSettings (files.deviceSettings, settings); ! written.has_value())
        content.handleError (written.error());
    reopenAudio();
    showRateSubstitution (settings.sampleRate);
}

void Workspace::showRateSubstitution (int requestedRate)
{
    // The Song's rate wins when the device offers it, and a mismatch has its own Notice.
    const auto openedRate = live->sampleRate();
    if (requestedRate <= 0 || openedRate <= 0 || openedRate == requestedRate || openedRate == openSongRate().value_or (0))
        return;
    if (song != nullptr && song->audioProblem().has_value())
        return;
    content.handleError (core::Error { "ui.device_rate_substituted",
                                       "the audio device opened at another rate than the one picked",
                                       { { "requested_rate", requestedRate }, { "opened_rate", openedRate }, { "device", live->calibrationKey().outputDevice } } });
}

void Workspace::reopenAudio()
{
    // Finishes a Pass (one the device cut short is already ended) before the device goes.
    detachAudio();
    live.reset();
    live = openAudio (openSongRate());
    watchDevices();
    if (song != nullptr)
    {
        song->attachAudio (live->audioBinding());
        showRateMismatch();
    }
    content.refresh();
    refreshSettingsPanels();
    if (live->engine() == nullptr)
        content.showNotice ("ui.no_audio_device");
    showLatencyNoticeIfUncalibrated();
}

void Workspace::watchDevices()
{
    // Never reopened from inside the LiveAudio's own callback: tick() answers it.
    live->onDeviceChange = [this] (engine::DeviceChange change) { pendingDeviceChange = change; };
}

void Workspace::tick (song::SongPackage::Clock::time_point now)
{
    answerSongRateChange();
    answerDeviceChange();
    pollLatencyRun (now);
}

void Workspace::answerSongRateChange()
{
    // The Song detached its audio when its rate changed (issue #160): the device is opened again
    // at the new rate when it offers it, as when a Song opens.
    if (song == nullptr || ! isIdle() || ! song->takeSampleRateChanged())
        return;
    core::logEvent (core::Level::info, "ui.audio_reopened", { { "change", "song_rate_changed" } });
    reopenAudio();
}

void Workspace::answerDeviceChange()
{
    if (! pendingDeviceChange.has_value())
        return;
    // A lost or reconfigured device is answered at once: a Pass it ran is already ended (the
    // window's poll finishes it first) and an export cannot finish without it. A device that came
    // back or appeared waits until nothing runs.
    const auto change = *pendingDeviceChange;
    const auto urgent = change == engine::DeviceChange::lost || change == engine::DeviceChange::reconfigured;
    const auto exporting = song != nullptr && song->isExporting();
    if (! isIdle() || (! urgent && exporting))
        return;

    pendingDeviceChange.reset();
    core::logEvent (core::Level::info, "ui.audio_reopened", { { "change", engine::deviceChangeName (change) } });
    reopenAudio();
    if (change == engine::DeviceChange::lost && live->engine() != nullptr)
        content.showNotice ("ui.audio_device_lost");
}

//==============================================================================
// Calibration (issue #39)

void Workspace::showLatencyNoticeIfUncalibrated()
{
    if (live->engine() != nullptr && ! live->storedCalibrationOffset().has_value())
        content.showNotice ("ui.latency_not_calibrated");
}

CalibrationStatus Workspace::calibrationStatus() const
{
    CalibrationStatus status;
    const auto& key = live->calibrationKey();
    status.deviceOpen = live->engine() != nullptr && key.sampleRate > 0;
    status.deviceText = key.inputDevice + " > " + key.outputDevice + ", " + std::to_string (key.sampleRate) + " Hz";
    status.sampleRate = key.sampleRate;
    status.latencyOffset = live->storedCalibrationOffset();
    if (song == nullptr)
        return status;

    status.cameraId = song->song().cameraDeviceId;
    for (const auto& device : song->cameraDevices())
        if (device.id == status.cameraId)
            status.cameraName = device.name;
    if (! status.cameraId.empty())
    {
        const auto stored = capture::findCameraCalibration (files.cameraCalibration, { status.cameraId, key.outputDevice });
        if (! stored.has_value())
            core::logError (stored.error(), core::Level::warn);
        status.cameraOffsetMs = stored.value_or (std::nullopt);
    }
    return status;
}

void Workspace::refreshCalibrationPanel()
{
    if (calibrationPanel != nullptr && ! latencyMeasuring())
        calibrationPanel->show (calibrationStatus());
}

bool Workspace::latencyMeasuring() const
{
    return latencyRun != nullptr || latencyOutcome.has_value();
}

void Workspace::measureLatency()
{
    if (! idleOrNotice() || calibrationPanel == nullptr)
        return;
    auto* engine = live->engine();
    if (engine == nullptr || live->sampleRate() <= 0)
        return content.showNotice ("ui.no_audio_device");

    // The run plays its own Song on the engine; the open Song is bound again when it ends.
    detachAudio();
    // Unique per run, not the next unused name: the run deletes its folder at start and end, and a
    // second process picking the same name would delete this one's (issue #187).
    const auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("overdub-calibration-loopback-" + juce::Uuid().toString());
    auto started = capture::LoopbackRun::start (*engine, live->recordLatency(), live->sampleRate(), folder);
    if (started.has_value())
        latencyRun = std::move (*started);
    else
        latencyOutcome = std::unexpected (started.error());
    calibrationPanel->showLatencyMeasuring();
}

void Workspace::pollLatencyRun (song::SongPackage::Clock::time_point now)
{
    if (latencyRun != nullptr)
    {
        auto polled = latencyRun->poll (now);
        if (! polled.has_value())
            return;
        latencyRun.reset();
        if (polled->has_value())
            latencyOutcome = (*polled)->measurement;
        else
            latencyOutcome = std::unexpected (polled->error());
    }
    if (! latencyOutcome.has_value())
        return;

    // Invariant (OpenSong::attachAudio): bind the Song again only once the audio thread has taken
    // the newest plan (the run's, or the empty one detachAudio left), so no plan renders the
    // Song's plugins while they are prepared again.
    if (auto* engine = live->engine(); engine != nullptr && engine->hasPendingPlan())
        return;
    const auto outcome = std::move (*latencyOutcome);
    latencyOutcome.reset();
    if (song != nullptr)
        song->attachAudio (live->audioBinding());
    content.refresh();

    // A failed run is shown in the panel only: logged once here, at the tier of its code.
    if (! outcome.has_value())
        core::logError (outcome.error(), tierFor (outcome.error().code) == Tier::notice ? core::Level::warn : core::Level::error);
    if (calibrationPanel != nullptr)
        calibrationPanel->showLatencyResult (outcome);
}

void Workspace::saveLatencyCalibration (song::SampleCount offsetSamples)
{
    if (! idleOrNotice() || live->engine() == nullptr)
        return;
    if (auto written = engine::writeCalibrationOffset (files.latencyCalibration, live->calibrationKey(), offsetSamples); ! written.has_value())
        return content.handleError (written.error());
    live->setCalibrationOffset (offsetSamples);
    // The LiveAudio owns the offset; the open Song takes it from there for its next Pass.
    if (song != nullptr)
        song->setCalibrationOffset (live->recordLatency().calibrationOffsetSamples);
    refreshCalibrationPanel();
}

void Workspace::measureClapPass()
{
    if (calibrationPanel == nullptr || song == nullptr || ! idleOrNotice())
        return;
    auto measured = capture::measureNewestVideoTake (song->song(), song->package().location());
    if (! measured.has_value())
        core::logError (measured.error(), core::Level::warn);
    else
        core::logEvent (core::Level::info,
                        "ui.clap_pass_measured",
                        { { "video_take_id", measured->videoTakeId }, { "camera_id", measured->cameraId }, { "output_device", measured->outputDevice }, { "applied_calibration_samples", measured->appliedCalibrationSamples }, { "report", capture::clapReportFields (measured->report) } });
    calibrationPanel->showClapResult (measured);
}

void Workspace::saveCameraCalibration (const capture::CameraCalibrationKey& key, double offsetMs)
{
    // key.outputDevice is the clap Pass's (its Video Take's), not necessarily the one open now.
    if (! idleOrNotice())
        return;
    if (auto written = capture::writeCameraCalibration (files.cameraCalibration, key, offsetMs); ! written.has_value())
        return content.handleError (written.error());
    // The refresh clears any result shown (CalibrationPanel::show), so the confirmation comes after it.
    refreshCalibrationPanel();
    if (calibrationPanel != nullptr && ! latencyMeasuring())
        calibrationPanel->showCameraSaved (key, offsetMs);
}
} // namespace overdub::ui
