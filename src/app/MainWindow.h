#pragma once

#include "app/LiveEngine.h"
#include "capture/Camera.h"
#include "control/EventTap.h"
#include "control/Server.h"
#include "plugin/AudioUnitLoader.h"
#include "ui/CalibrationPanel.h"
#include "ui/DeviceSettingsPanel.h"
#include "ui/MainComponent.h"
#include "ui/Workspace.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <vector>

namespace overdub::app
{
// The one top-level window (issue #27). Closing it quits the app. It owns the camera (issue #35),
// the content (ui::MainComponent) and the ui::Workspace, which holds the live audio and the open
// Song and runs the App Intents (issue #170). The window keeps what needs native dialogs and
// windows: the File menu (New Song, Open, Duplicate, whose dialogs' answers run App Intents, and
// Export..., which opens the Export sheet), Undo / Redo in the Edit menu, the settings window with
// the audio device picker and the calibration panel (per machine), which the Workspace shows and
// hides through onShowSettings, and quitting (onQuit). While a Pass or a loopback calibration
// runs, the menu items that would interrupt it are disabled.
class MainWindow final : public juce::DocumentWindow, private juce::MenuBarModel
{
public:
    // The control socket starts once the Workspace exists, from `controlConfig`, streaming
    // `eventTap` (installed on the process logger by the caller).
    MainWindow (const juce::String& appName,
                std::unique_ptr<LiveEngine> liveEngine,
                const control::ServerConfig& controlConfig,
                std::shared_ptr<control::EventTap> eventTap);
    ~MainWindow() override;

    void closeButtonPressed() override;
    // Runs quit, which is refused (with a Notice) while a Separation runs (ui/Availability.h).
    void requestQuit();

private:
    juce::StringArray getMenuBarNames() override;
    juce::PopupMenu getMenuForIndex (int topLevelMenuIndex, const juce::String& menuName) override;
    void menuItemSelected (int menuItemID, int topLevelMenuIndex) override;

    void newSong();
    void chooseSongToOpen();
    void duplicateSong();
    void showSettingsWindow (bool shown);
    // File > Convert Sample Rate: one item per rate the open device offers (offeredMenuRates).
    juce::PopupMenu convertSampleRateMenu();
    void updateTitle();

    juce::String appName;
    // Creates every open Song's AUs out-of-process (issue #33); outlives the Workspace.
    plugin::AudioUnitLoader pluginLoader;
    // Outlives the Workspace, whose open Song uses it until it closes.
    std::unique_ptr<capture::Camera> camera;
    ui::MainComponent content;
    // Destroyed in the destructor, while the content and the settings panels still exist.
    std::unique_ptr<ui::Workspace> workspace;
    juce::TimedCallback workspaceTick { [this] { workspace->tick(); } };
    std::unique_ptr<juce::FileChooser> chooser;
    std::vector<int> offeredMenuRates; // as the File menu last listed them
    std::unique_ptr<juce::DocumentWindow> settingsWindow;
    // Invariant: declared last and reset first in the destructor, because a request's handler may
    // reach any member above. Null when disabled or when it could not start.
    std::unique_ptr<control::Server> controlServer;
    juce::TimedCallback controlTick { [this] {
        const auto now = control::Clock::now();
        controlServer->pump (now);
        content.controllingProcesses = controlServer->clientProcesses (now);
    } };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
};
} // namespace overdub::app
