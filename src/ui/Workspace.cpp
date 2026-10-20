#include "ui/Workspace.h"

#include "capture/ClapSelftest.h"
#include "core/Described.h"
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
    content.runAppIntent = [this] (const AppIntent& intent) { return run (intent); };
    watchDevices();
    if (live->engine() == nullptr)
        content.showNotice ("ui.no_audio_device");
    showLatencyNoticeIfUncalibrated();
    calibrationState.status = calibrationStatus();
}

Workspace::~Workspace()
{
    content.runAppIntent = nullptr;
    onOpenSongChanged = nullptr; // the owner is being torn down too
    latencyRun.reset();
    closeSong();
}

bool Workspace::isIdle() const
{
    return ! runningCode().has_value();
}

std::optional<std::string> Workspace::runningCode() const
{
    if (latencyMeasuring())
        return "ui.calibration_running";
    if (song == nullptr)
        return std::nullopt;
    if (song->isSeparating())
        return "ui.separation_running";
    if (song->isConverting())
        return "ui.conversion_running";
    if (song->isRecording())
        return "ui.stop_recording_first";
    return std::nullopt;
}

bool Workspace::idleOrNotice()
{
    const auto running = runningCode();
    if (running.has_value())
        content.showNotice (*running);
    return ! running.has_value();
}

IntentResult Workspace::refuseWhileRunning (const std::string& code, std::string_view op)
{
    return presented (core::Error { code, "waits until what runs has ended", { { "op", std::string (op) } } });
}

IntentResult Workspace::presented (core::Error error)
{
    content.handleError (error);
    return std::unexpected (std::move (error));
}

//==============================================================================
// App Intents (issue #170)

namespace
{
juce::File fileAt (const core::AbsolutePath& path)
{
    return juce::File (juce::String::fromUTF8 (path.value.c_str()));
}

bool waitsForIdle (const AppIntent& intent)
{
    if (std::holds_alternative<intent::Quit> (intent))
        return false;
    if (const auto* settings = std::get_if<intent::ShowSettings> (&intent))
        return settings->shown;
    return true;
}
} // namespace

IntentResult Workspace::run (const AppIntent& appIntent)
{
    using namespace intent;
    if (const auto running = runningCode(); running.has_value() && waitsForIdle (appIntent))
        return refuseWhileRunning (*running, core::operationName (appIntent));
    return core::visitExhaustively (
        appIntent,
        core::Overloaded {
            [this] (const NewSongAt& create) { return newSongAt (fileAt (create.path)); },
            [this] (const OpenSongAt& open) { return openSongFrom (fileAt (open.path)); },
            [this] (const DuplicateSongTo& duplicate) { return duplicateSongTo (fileAt (duplicate.path)); },
            [this] (const ConvertSampleRate& convert) { return convertSampleRate (convert.rate); },
            [this] (const ShowSettings& settings) { return showSettings (settings.shown); },
            [this] (const ApplyDeviceSettings& apply) {
        return applyDeviceSettings (engine::DeviceSettings { apply.deviceType, apply.inputDevice, apply.outputDevice, apply.sampleRate, apply.bufferSize });
    },
            [this] (const MeasureLatency&) { return measureLatency(); },
            [this] (const SaveLatencyCalibration& save) { return saveLatencyCalibration (save.offsetSamples); },
            [this] (const MeasureClapPass&) { return measureClapPass(); },
            [this] (const SaveCameraCalibration& save) { return saveCameraCalibration ({ save.cameraId, save.outputDevice }, save.offsetMs); },
            [this] (const Quit&) { return quit(); },
        });
}

IntentResult Workspace::quit()
{
    if (song != nullptr && song->isSeparating())
        return refuseWhileRunning ("ui.separation_running", intent::Quit::op);
    if (onQuit)
        onQuit();
    return IntentDone {};
}

IntentResult Workspace::showSettings (bool shown)
{
    if (onShowSettings)
        onShowSettings (shown);
    if (shown)
        refreshSettingsPanels();
    return IntentDone {};
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
        swapAudio (songRate);
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
    refreshCalibration();
    if (onOpenSongChanged)
        onOpenSongChanged();
}

IntentResult Workspace::newSongAt (const juce::File& chosen)
{
    auto created = createSongAt (chosen, live->sampleRate());
    if (! created.has_value())
        return presented (created.error());
    showSong (std::move (*created), {});
    return IntentDone {};
}

IntentResult Workspace::openSongFrom (const juce::File& chosen)
{
    // A package is never open twice: opening the open Song again changes nothing.
    if (song != nullptr && song->package().location() == packageLocationOpened (chosen))
        return IntentDone {};
    auto opened = openSongAt (chosen);
    if (! opened.has_value())
        return presented (opened.error());
    showSong (std::move (opened->package), opened->notices);
    return IntentDone {};
}

IntentResult Workspace::duplicateSongTo (const juce::File& chosen)
{
    if (song == nullptr)
        return content.quietError (noSongOpen (intent::DuplicateSongTo::op));
    auto duplicated = ui::duplicateSongTo (song->package(), chosen);
    if (! duplicated.has_value())
        return presented (duplicated.error());
    return openSongFrom (*duplicated);
}

IntentResult Workspace::convertSampleRate (int toRate)
{
    if (content.songView() == nullptr)
        return content.quietError (noSongOpen (intent::ConvertSampleRate::op));
    return content.songView()->convertSampleRate (toRate);
}

void Workspace::closeSong()
{
    detachAudio();
    content.setOpenSong (nullptr);
    song.reset();
    calibrationState.clap = {};
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
        devicePanel->onApply = [this] (const engine::DeviceSettings& settings) {
            content.run (AppIntent { intent::ApplyDeviceSettings { settings.deviceType, settings.inputDevice, settings.outputDevice, settings.sampleRate, settings.bufferSize } });
        };
        devicePanel->offerForDevices = [this] (const std::string& input, const std::string& output) { return live->offerFor (input, output); };
    }
    if (calibrationPanel != nullptr)
        calibrationPanel->onIntent = [this] (const Intent& intent) { content.run (intent); };
    refreshSettingsPanels();
}

void Workspace::refreshSettingsPanels()
{
    if (devicePanel != nullptr)
        devicePanel->show (live->deviceChoices());
    refreshCalibration();
}

IntentResult Workspace::applyDeviceSettings (const engine::DeviceSettings& settings)
{
    core::logEvent (core::Level::info, "ui.device_choice_changed", engine::deviceSettingsFields (settings));

    // The engine's shape is fixed per device (engine/Engine.h), so a new choice opens a new
    // LiveAudio, which opens the saved choice (or falls back to the default and logs why).
    if (auto written = engine::writeDeviceSettings (files.deviceSettings, settings); ! written.has_value())
        content.handleError (written.error());
    reopenAudio();
    showRateSubstitution (settings.sampleRate);
    return IntentDone {};
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
    swapAudio (openSongRate());
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

void Workspace::swapAudio (std::optional<int> songRate)
{
    live.reset();
    live = openAudio (songRate);
    watchDevices();
    calibrationState.latency = {};
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

void Workspace::refreshCalibration()
{
    calibrationState.status = calibrationStatus();
    if (calibrationPanel != nullptr)
        calibrationPanel->show (calibrationState);
}

bool Workspace::latencyMeasuring() const
{
    return latencyRun != nullptr || latencyOutcome.has_value();
}

IntentResult Workspace::measureLatency()
{
    auto* engine = live->engine();
    if (engine == nullptr || live->sampleRate() <= 0)
        return presented (core::Error { "ui.no_audio_device", "no audio device is open to measure", { { "op", std::string (intent::MeasureLatency::op) } } });

    // The run plays its own Song on the engine; the open Song is bound again when it ends.
    detachAudio();
    const auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory).getNonexistentChildFile ("overdub-calibration-loopback", "", false);
    auto started = capture::LoopbackRun::start (*engine, live->recordLatency(), live->sampleRate(), folder);
    if (started.has_value())
        latencyRun = std::move (*started);
    else
        latencyOutcome = std::unexpected (started.error());
    calibrationState.latency = LatencyMeasuring {};
    refreshCalibration();
    return IntentDone {};
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
    auto outcome = std::move (*latencyOutcome);
    latencyOutcome.reset();
    if (song != nullptr)
        song->attachAudio (live->audioBinding());
    content.refresh();

    // A failed run is shown in the CalibrationState only: logged once here, at the tier of its code.
    if (! outcome.has_value())
        core::logError (outcome.error(), tierFor (outcome.error().code) == Tier::notice ? core::Level::warn : core::Level::error);
    else
        core::logEvent (core::Level::info, "ui.latency_measured", { { "offset_samples", outcome->offsetSamples }, { "jitter_samples", outcome->jitterSamples }, { "passed", capture::loopbackPassed (*outcome) } });
    calibrationState.latency = latencyMeasured (std::move (outcome), live->storedCalibrationOffset());
    refreshCalibration();
}

IntentResult Workspace::saveLatencyCalibration (song::SampleCount offsetSamples)
{
    if (live->engine() == nullptr)
        return presented (core::Error { "ui.no_audio_device", "no audio device is open to calibrate", { { "op", std::string (intent::SaveLatencyCalibration::op) } } });
    if (auto written = engine::writeCalibrationOffset (files.latencyCalibration, live->calibrationKey(), offsetSamples); ! written.has_value())
        return presented (written.error());
    live->setCalibrationOffset (offsetSamples);
    // The LiveAudio owns the offset; the open Song takes it from there for its next Pass.
    if (song != nullptr)
        song->setCalibrationOffset (live->recordLatency().calibrationOffsetSamples);
    calibrationState.latency = {};
    refreshCalibration();
    return IntentDone {};
}

IntentResult Workspace::measureClapPass()
{
    if (song == nullptr)
        return content.quietError (noSongOpen (intent::MeasureClapPass::op));
    auto measured = capture::measureNewestVideoTake (song->song(), song->package().location());
    if (! measured.has_value())
        core::logError (measured.error(), core::Level::warn);
    else
        core::logEvent (core::Level::info,
                        "ui.clap_pass_measured",
                        { { "video_take_id", measured->videoTakeId }, { "camera_id", measured->cameraId }, { "output_device", measured->outputDevice }, { "applied_calibration_samples", measured->appliedCalibrationSamples }, { "report", capture::clapReportFields (measured->report) } });
    calibrationState.clap = clapMeasured (std::move (measured));
    refreshCalibration();
    return IntentDone {};
}

IntentResult Workspace::saveCameraCalibration (const capture::CameraCalibrationKey& key, double offsetMs)
{
    if (auto written = capture::writeCameraCalibration (files.cameraCalibration, key, offsetMs); ! written.has_value())
        return presented (written.error());
    calibrationState.clap = CameraSaved { intent::SaveCameraCalibration { key.cameraId, key.outputDevice, offsetMs } };
    refreshCalibration();
    return IntentDone {};
}
} // namespace overdub::ui
