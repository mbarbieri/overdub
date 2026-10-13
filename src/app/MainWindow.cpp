#include "app/MainWindow.h"

#include "capture/AvCamera.h"
#include "capture/CameraCalibration.h"
#include "capture/ClapSelftest.h"
#include "core/Log.h"
#include "engine/LatencyCalibration.h"
#include "ui/Layout.h"
#include "ui/SongFiles.h"
#include "ui/UserMessages.h"

namespace overdub::app
{
namespace
{
enum MenuItem
{
    newSongItem = 1,
    openSongItem,
    duplicateSongItem,
    settingsItem,
    undoItem,
    redoItem
};

juce::File defaultSongFolder()
{
    return juce::File::getSpecialLocation (juce::File::userMusicDirectory);
}

constexpr int devicePanelHeight = 240;
constexpr int calibrationPanelHeight = 520;
constexpr int settingsWidth = 460;
constexpr int latencyPollIntervalMs = 50;
constexpr int deviceChangePollIntervalMs = 100;

// The settings window's content: the device picker above the calibration panel.
class SettingsContent final : public juce::Component
{
public:
    SettingsContent()
    {
        addAndMakeVisible (devices);
        addAndMakeVisible (calibration);
    }

    void resized() override
    {
        auto area = getLocalBounds();
        devices.setBounds (area.removeFromTop (devicePanelHeight));
        calibration.setBounds (area);
    }

    ui::DeviceSettingsPanel devices;
    ui::CalibrationPanel calibration;
};

// The settings window, hidden (not destroyed) when closed.
class SettingsWindow final : public juce::DocumentWindow
{
public:
    explicit SettingsWindow (std::unique_ptr<SettingsContent> content)
        : DocumentWindow ("Settings", juce::Colours::black, juce::DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (content.release(), false);
        centreWithSize (settingsWidth, devicePanelHeight + calibrationPanelHeight);
    }

    void closeButtonPressed() override { setVisible (false); }
};

juce::PopupMenu::Item historyItem (int id, const juce::String& verb, bool enabled, const std::string& label, const char* shortcut)
{
    juce::PopupMenu::Item item { enabled ? verb + " " + juce::String::fromUTF8 (label.c_str()) : verb };
    item.setID (id).setEnabled (enabled);
    item.shortcutKeyDescription = juce::String (juce::CharPointer_UTF8 (shortcut));
    return item;
}
} // namespace

MainWindow::MainWindow (const juce::String& name, std::unique_ptr<LiveEngine> liveEngine)
    : DocumentWindow (name,
                      juce::Desktop::getInstance().getDefaultLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId),
                      juce::DocumentWindow::allButtons),
      appName (name),
      live (std::move (liveEngine)),
      camera (capture::makeAvCamera())
{
    content.onNewSong = [this] { newSong(); };
    content.onOpenSong = [this] { chooseSongToOpen(); };
    // Discovery instantiates nothing and is quick; it runs on each "+ Insert" so new installs show.
    content.listInsertPlugins = [this] { return pluginLoader.findEffects(); };

    setUsingNativeTitleBar (true);
    setContentNonOwned (&content, false);
    setResizable (true, true);
    setResizeLimits (ui::minimumWindowWidth, ui::minimumWindowHeight, 10000, 10000);
    centreWithSize (1200, 800);

    juce::PopupMenu appleMenuExtras;
    appleMenuExtras.addItem (settingsItem, "Settings...");
    juce::MenuBarModel::setMacMainMenu (this, &appleMenuExtras);

    watchDevices();
    setVisible (true);
    content.grabKeyboardFocus();
    if (live->engine() == nullptr)
        content.showNotice ("ui.no_audio_device");
    showLatencyNoticeIfUncalibrated();
}

MainWindow::~MainWindow()
{
    juce::MenuBarModel::setMacMainMenu (nullptr);
    deviceChangePoll.stopTimer();
    latencyRunPoll.stopTimer();
    latencyRun.reset();
    settingsPanel = nullptr;
    calibrationPanel = nullptr;
    settingsWindow.reset();
    closeSong();
    clearContentComponent();
}

void MainWindow::closeButtonPressed()
{
    juce::JUCEApplication::getInstance()->systemRequestedQuit();
}

bool MainWindow::passRunning() const
{
    return openSong != nullptr && openSong->isRecording();
}

bool MainWindow::idleOrNotice()
{
    if (latencyMeasuring())
    {
        content.showNotice ("ui.calibration_running");
        return false;
    }
    if (! passRunning())
        return true;
    content.showNotice ("ui.stop_recording_first");
    return false;
}

//==============================================================================
// Menus

juce::StringArray MainWindow::getMenuBarNames()
{
    return { "File", "Edit" };
}

juce::PopupMenu MainWindow::getMenuForIndex (int topLevelMenuIndex, const juce::String&)
{
    juce::PopupMenu menu;
    const auto idle = ! passRunning() && ! latencyMeasuring();
    if (topLevelMenuIndex == 0)
    {
        menu.addItem (newSongItem, "New Song...", idle);
        menu.addItem (openSongItem, "Open...", idle);
        menu.addItem (duplicateSongItem, "Duplicate...", idle && openSong != nullptr);
        menu.addSeparator();
        menu.addItem (settingsItem, "Settings...", idle);
        return menu;
    }

    if (openSong == nullptr)
    {
        menu.addItem (historyItem (undoItem, "Undo", false, {}, "\xe2\x8c\x98Z"));
        menu.addItem (historyItem (redoItem, "Redo", false, {}, "\xe2\x87\xa7\xe2\x8c\x98Z"));
        return menu;
    }
    const auto& package = openSong->package();
    menu.addItem (historyItem (undoItem, "Undo", idle && package.canUndo(), package.undoLabel(), "\xe2\x8c\x98Z"));
    menu.addItem (historyItem (redoItem, "Redo", idle && package.canRedo(), package.redoLabel(), "\xe2\x87\xa7\xe2\x8c\x98Z"));
    return menu;
}

void MainWindow::menuItemSelected (int menuItemID, int)
{
    switch (menuItemID)
    {
        case newSongItem: newSong(); break;
        case openSongItem: chooseSongToOpen(); break;
        case duplicateSongItem: duplicateSong(); break;
        case settingsItem: showSettings(); break;
        case undoItem: content.perform (ui::Command::undo); break;
        case redoItem: content.perform (ui::Command::redo); break;
        default: break;
    }
}

//==============================================================================
// Songs. Each dialog's callback checks again: a Pass may have started while it was open.

void MainWindow::newSong()
{
    if (! idleOrNotice())
        return;
    chooser = std::make_unique<juce::FileChooser> ("New Song", defaultSongFolder().getChildFile ("Untitled.overdub"), "*.overdub");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles, [this] (const juce::FileChooser& finished) {
        const auto chosen = finished.getResult();
        if (chosen == juce::File() || ! idleOrNotice())
            return;
        auto created = ui::createSongAt (chosen, live->sampleRate());
        if (! created.has_value())
            return content.handleError (created.error());
        showSong (std::move (*created), {});
    });
}

void MainWindow::chooseSongToOpen()
{
    if (! idleOrNotice())
        return;
    chooser = std::make_unique<juce::FileChooser> ("Open Song", defaultSongFolder(), "*.overdub");
    const auto flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories | juce::FileBrowserComponent::canSelectFiles;
    chooser->launchAsync (flags, [this] (const juce::FileChooser& finished) {
        const auto chosen = finished.getResult();
        if (chosen == juce::File() || ! idleOrNotice())
            return;
        openSongFrom (chosen);
    });
}

void MainWindow::duplicateSong()
{
    if (openSong == nullptr || ! idleOrNotice())
        return;
    const auto current = openSong->package().location();
    chooser = std::make_unique<juce::FileChooser> ("Duplicate Song", current.getSiblingFile (current.getFileNameWithoutExtension() + " copy.overdub"), "*.overdub");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles, [this] (const juce::FileChooser& finished) {
        const auto chosen = finished.getResult();
        if (chosen == juce::File() || openSong == nullptr || ! idleOrNotice())
            return;
        auto duplicated = ui::duplicateSongTo (openSong->package(), chosen);
        if (! duplicated.has_value())
            return content.handleError (duplicated.error());
        // The duplicate becomes the open Song; the original stays as it was.
        openSongFrom (*duplicated);
    });
}

void MainWindow::openSongFrom (const juce::File& chosen)
{
    // A package is never open twice: opening the open Song again changes nothing.
    if (openSong != nullptr && openSong->package().location() == ui::packageLocationOpened (chosen))
        return;
    auto opened = ui::openSongAt (chosen);
    if (! opened.has_value())
        return content.handleError (opened.error());
    showSong (std::move (opened->package), opened->notices);
}

void MainWindow::showSong (std::unique_ptr<song::SongPackage> package, const std::vector<song::Notice>& notices)
{
    closeSong();
    // The Song's rate is fixed (ADR 0004): a device that offers it is switched to it.
    const auto songRate = package->song().sampleRate;
    const auto rateSwitch = engine::sampleRateForSong (live->sampleRate(), songRate, live->deviceChoices().sampleRates).has_value();
    if (rateSwitch)
    {
        live.reset();
        live = std::make_unique<LiveEngine> (songRate);
        watchDevices();
        if (settingsPanel != nullptr)
            settingsPanel->show (live->deviceChoices());
    }
    openSong = std::make_unique<ui::OpenSong> (std::move (package), live->audioBinding(), pluginLoader.instantiator());
    showRateMismatch();
    if (rateSwitch)
        showLatencyNoticeIfUncalibrated();
    // A Song saved with its camera Armed opens it now; without camera access that is a Notice.
    if (auto attached = openSong->attachCamera (camera.get(), capture::defaultCameraCalibrationFile()); ! attached.has_value())
        content.handleError (attached.error());
    content.setOpenSong (openSong.get());
    for (const auto& notice : notices)
        content.showNotice (notice.code);
    updateTitle();
    refreshCalibrationPanel();
    content.grabKeyboardFocus();
}

void MainWindow::detachAudio()
{
    if (openSong == nullptr)
        return;
    for (const auto& error : openSong->detachAudio())
        content.handleError (error);
}

void MainWindow::closeSong()
{
    detachAudio();
    content.setOpenSong (nullptr);
    openSong.reset();
    updateTitle();
}

void MainWindow::updateTitle()
{
    if (openSong == nullptr)
        setName (appName);
    else
        setName (appName + juce::String (juce::CharPointer_UTF8 (" \xe2\x80\x94 ")) + openSong->package().location().getFileNameWithoutExtension());
}

//==============================================================================
// Settings

void MainWindow::showSettings()
{
    if (! idleOrNotice())
        return;
    if (settingsWindow == nullptr)
    {
        auto settingsContent = std::make_unique<SettingsContent>();
        settingsPanel = &settingsContent->devices;
        calibrationPanel = &settingsContent->calibration;
        settingsPanel->onApply = [this] (const engine::DeviceSettings& settings) { applyDeviceSettings (settings); };
        calibrationPanel->onMeasureLatency = [this] { measureLatency(); };
        calibrationPanel->onSaveLatency = [this] (song::SampleCount offset) { saveLatencyCalibration (offset); };
        calibrationPanel->onMeasureClap = [this] { measureClapPass(); };
        calibrationPanel->onSaveCamera = [this] (const capture::CameraCalibrationKey& key, double offsetMs) { saveCameraCalibration (key, offsetMs); };
        settingsWindow = std::make_unique<SettingsWindow> (std::move (settingsContent));
    }
    settingsPanel->show (live->deviceChoices());
    refreshCalibrationPanel();
    settingsWindow->setVisible (true);
    settingsWindow->toFront (true);
}

void MainWindow::applyDeviceSettings (const engine::DeviceSettings& settings)
{
    if (! idleOrNotice())
        return;
    core::logEvent (core::Level::info, "app.device_choice_changed", engine::deviceSettingsFields (settings));

    // The engine's shape is fixed per device (engine/Engine.h), so a new choice builds a new
    // LiveEngine, which opens the saved choice (or falls back to the default and logs why).
    if (auto written = engine::writeDeviceSettings (engine::defaultDeviceSettingsFile(), settings); ! written.has_value())
        content.handleError (written.error());
    reopenAudio();
}

std::optional<int> MainWindow::openSongRate() const
{
    return openSong != nullptr ? std::optional<int> (openSong->song().sampleRate) : std::nullopt;
}

void MainWindow::reopenAudio()
{
    // Finishes a Pass (one the device cut short is already ended) before the device goes.
    detachAudio();
    live.reset();
    live = std::make_unique<LiveEngine> (openSongRate());
    watchDevices();
    if (openSong != nullptr)
    {
        openSong->attachAudio (live->audioBinding());
        showRateMismatch();
    }
    content.refresh();

    if (settingsPanel != nullptr)
        settingsPanel->show (live->deviceChoices());
    refreshCalibrationPanel();
    if (live->engine() == nullptr)
        content.showNotice ("ui.no_audio_device");
    showLatencyNoticeIfUncalibrated();
}

void MainWindow::showRateMismatch()
{
    // ui.no_audio_device has its own Notice; only the rate is said here.
    if (const auto problem = openSong->audioProblem(); problem.has_value() && problem->code == "ui.sample_rate_mismatch")
        content.handleError (*problem);
}

void MainWindow::watchDevices()
{
    // Never rebuilt from inside the LiveEngine's own callback: the poll answers it.
    live->onDeviceChange = [this] (engine::DeviceChange change) {
        pendingDeviceChange = change;
        deviceChangePoll.startTimer (deviceChangePollIntervalMs);
    };
}

void MainWindow::applyDeviceChange()
{
    if (! pendingDeviceChange.has_value())
    {
        deviceChangePoll.stopTimer();
        return;
    }

    // A lost or reconfigured device is answered at once: a Pass it ran is already ended (the
    // window's poll finishes it first) and an export cannot finish without it. A device that came
    // back or appeared waits until nothing runs.
    const auto change = *pendingDeviceChange;
    const auto urgent = change == engine::DeviceChange::lost || change == engine::DeviceChange::reconfigured;
    if (latencyMeasuring() || passRunning() || (! urgent && openSong != nullptr && openSong->isExporting()))
        return;

    deviceChangePoll.stopTimer();
    pendingDeviceChange.reset();
    core::logEvent (core::Level::info, "app.audio_reopened", { { "change", engine::deviceChangeName (change) } });
    reopenAudio();
    if (change == engine::DeviceChange::lost && live->engine() != nullptr)
        content.showNotice ("ui.audio_device_lost");
}

//==============================================================================
// Calibration (issue #39)

void MainWindow::showLatencyNoticeIfUncalibrated()
{
    if (live->engine() != nullptr && ! live->storedCalibrationOffset().has_value())
        content.showNotice ("ui.latency_not_calibrated");
}

ui::CalibrationStatus MainWindow::calibrationStatus() const
{
    ui::CalibrationStatus status;
    const auto& key = live->calibrationKey();
    status.deviceOpen = live->engine() != nullptr && key.sampleRate > 0;
    status.deviceText = key.inputDevice + " > " + key.outputDevice + ", " + std::to_string (key.sampleRate) + " Hz";
    status.sampleRate = key.sampleRate;
    status.latencyOffset = live->storedCalibrationOffset();
    if (openSong == nullptr)
        return status;

    status.cameraId = openSong->song().cameraDeviceId;
    for (const auto& device : openSong->cameraDevices())
        if (device.id == status.cameraId)
            status.cameraName = device.name;
    if (! status.cameraId.empty())
    {
        const auto stored = capture::findCameraCalibration (capture::defaultCameraCalibrationFile(), { status.cameraId, key.outputDevice });
        if (! stored.has_value())
            core::logError (stored.error(), core::Level::warn);
        status.cameraOffsetMs = stored.value_or (std::nullopt);
    }
    return status;
}

void MainWindow::refreshCalibrationPanel()
{
    if (calibrationPanel != nullptr && ! latencyMeasuring())
        calibrationPanel->show (calibrationStatus());
}

bool MainWindow::latencyMeasuring() const
{
    return latencyRun != nullptr || latencyOutcome.has_value();
}

void MainWindow::measureLatency()
{
    if (! idleOrNotice() || calibrationPanel == nullptr)
        return;
    auto* engine = live->engine();
    if (engine == nullptr || live->sampleRate() <= 0)
        return content.showNotice ("ui.no_audio_device");

    // The run plays its own Song on the engine; the open Song is bound again when it ends.
    detachAudio();
    const auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory).getNonexistentChildFile ("overdub-calibration-loopback", "", false);
    auto started = capture::LoopbackRun::start (*engine, live->recordLatency(), live->sampleRate(), folder);
    if (started.has_value())
        latencyRun = std::move (*started);
    else
        latencyOutcome = std::unexpected (started.error());
    calibrationPanel->showLatencyMeasuring();
    latencyRunPoll.startTimer (latencyPollIntervalMs);
}

void MainWindow::pollLatencyRun()
{
    if (latencyRun != nullptr)
    {
        auto polled = latencyRun->poll();
        if (! polled.has_value())
            return;
        latencyRun.reset();
        if (polled->has_value())
            latencyOutcome = (*polled)->measurement;
        else
            latencyOutcome = std::unexpected (polled->error());
    }
    if (! latencyOutcome.has_value())
        return latencyRunPoll.stopTimer();

    // Invariant (OpenSong::attachAudio): bind the Song again only once the audio thread has taken
    // the newest plan (the run's, or the empty one detachAudio left), so no plan renders the
    // Song's plugins while they are prepared again.
    if (auto* engine = live->engine(); engine != nullptr && engine->hasPendingPlan())
        return;
    latencyRunPoll.stopTimer();
    const auto outcome = std::move (*latencyOutcome);
    latencyOutcome.reset();
    if (openSong != nullptr)
        openSong->attachAudio (live->audioBinding());
    content.refresh();

    // A failed run is shown in the panel only: logged once here, at the tier of its code.
    if (! outcome.has_value())
        core::logError (outcome.error(), ui::tierFor (outcome.error().code) == ui::Tier::notice ? core::Level::warn : core::Level::error);
    if (calibrationPanel != nullptr)
        calibrationPanel->showLatencyResult (outcome);
}

void MainWindow::saveLatencyCalibration (song::SampleCount offsetSamples)
{
    if (! idleOrNotice() || live->engine() == nullptr)
        return;
    if (auto written = engine::writeCalibrationOffset (engine::defaultLatencyCalibrationFile(), live->calibrationKey(), offsetSamples); ! written.has_value())
        return content.handleError (written.error());
    live->setCalibrationOffset (offsetSamples);
    if (openSong != nullptr)
        openSong->setCalibrationOffset (offsetSamples);
    refreshCalibrationPanel();
}

void MainWindow::measureClapPass()
{
    if (calibrationPanel == nullptr || openSong == nullptr || ! idleOrNotice())
        return;
    auto measured = capture::measureNewestVideoTake (openSong->song(), openSong->package().location());
    if (! measured.has_value())
        core::logError (measured.error(), core::Level::warn);
    else
        core::logEvent (core::Level::info,
                        "app.clap_pass_measured",
                        { { "video_take_id", measured->videoTakeId }, { "camera_id", measured->cameraId }, { "output_device", measured->outputDevice }, { "applied_calibration_samples", measured->appliedCalibrationSamples }, { "report", capture::clapReportFields (measured->report) } });
    calibrationPanel->showClapResult (measured);
}

void MainWindow::saveCameraCalibration (const capture::CameraCalibrationKey& key, double offsetMs)
{
    // key.outputDevice is the clap Pass's (its Video Take's), not necessarily the one open now.
    if (! idleOrNotice())
        return;
    if (auto written = capture::writeCameraCalibration (capture::defaultCameraCalibrationFile(), key, offsetMs); ! written.has_value())
        return content.handleError (written.error());
    refreshCalibrationPanel();
    if (calibrationPanel != nullptr && ! latencyMeasuring())
        calibrationPanel->showCameraSaved (key, offsetMs);
}
} // namespace overdub::app
