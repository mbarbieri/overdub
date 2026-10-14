#pragma once

#include "app/LiveEngine.h"
#include "capture/Camera.h"
#include "plugin/AudioUnitLoader.h"
#include "ui/CalibrationPanel.h"
#include "ui/DeviceSettingsPanel.h"
#include "ui/MainComponent.h"
#include "ui/Workspace.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

namespace overdub::app
{
// The one top-level window (issue #27). Closing it quits the app. It owns the camera (issue #35),
// the content (ui::MainComponent) and the ui::Workspace, which holds the live audio and the open
// Song and answers device changes and calibrations (issue #105). The window keeps what needs native
// dialogs and windows: the File menu (New Song, Open, Duplicate), Undo / Redo in the Edit menu, and
// the settings window with the audio device picker and the calibration panel (per machine). While
// a Pass or a loopback calibration runs, the menu items that would interrupt it are disabled.
class MainWindow final : public juce::DocumentWindow, private juce::MenuBarModel
{
public:
    MainWindow (const juce::String& appName, std::unique_ptr<LiveEngine> liveEngine);
    ~MainWindow() override;

    void closeButtonPressed() override;
    // ui::Workspace::mayQuitOrNotice: false (with a Notice) while a Separation runs.
    bool mayQuitOrNotice();

private:
    juce::StringArray getMenuBarNames() override;
    juce::PopupMenu getMenuForIndex (int topLevelMenuIndex, const juce::String& menuName) override;
    void menuItemSelected (int menuItemID, int topLevelMenuIndex) override;

    void newSong();
    void chooseSongToOpen();
    void duplicateSong();
    void showSettings();
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
    std::unique_ptr<juce::DocumentWindow> settingsWindow;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
};
} // namespace overdub::app
