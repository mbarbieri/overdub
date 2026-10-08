#pragma once

#include "app/LiveEngine.h"
#include "capture/Camera.h"
#include "plugin/AudioUnitLoader.h"
#include "song/SongPackage.h"
#include "ui/DeviceSettingsPanel.h"
#include "ui/MainComponent.h"
#include "ui/OpenSong.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

namespace overdub::app
{
// The one top-level window (issue #27). Closing it quits the app. It owns the live audio, the
// camera (issue #35, attached to each open Song), the open Song (a ui::OpenSong) and the content (ui::MainComponent), and runs what needs native
// dialogs: the File menu (New Song, Open, Duplicate), Undo / Redo in the Edit menu, and the
// settings window with the audio device picker (per machine).
//
// Opening another Song, changing the device and closing the window each finish a running Pass
// first (ui::OpenSong's invariant). While a Pass runs, those menu items are disabled.
class MainWindow final : public juce::DocumentWindow, private juce::MenuBarModel
{
public:
    MainWindow (const juce::String& appName, std::unique_ptr<LiveEngine> liveEngine);
    ~MainWindow() override;

    void closeButtonPressed() override;

private:
    juce::StringArray getMenuBarNames() override;
    juce::PopupMenu getMenuForIndex (int topLevelMenuIndex, const juce::String& menuName) override;
    void menuItemSelected (int menuItemID, int topLevelMenuIndex) override;

    void newSong();
    void chooseSongToOpen();
    void duplicateSong();
    void showSettings();
    void applyDeviceSettings (const engine::DeviceSettings& settings);

    // Closes the open Song (finishing a running Pass) and shows this one.
    void showSong (std::unique_ptr<song::SongPackage> package, const std::vector<song::Notice>& notices);
    void openSongFrom (const juce::File& chosen);
    void closeSong();
    // Finishes a running Pass and unbinds the engine; errors go to the content's Alerts.
    void detachAudio();
    bool passRunning() const;
    // True when no Pass runs; otherwise shows the "stop recording first" Notice.
    bool idleOrNotice();
    void updateTitle();

    juce::String appName;
    // Creates every open Song's AUs out-of-process (issue #33); outlives them.
    plugin::AudioUnitLoader pluginLoader;
    // Invariant: declared before `openSong`, so the device keeps running while a Pass finishes.
    std::unique_ptr<LiveEngine> live;
    // Declared before `openSong`, which uses it until it closes.
    std::unique_ptr<capture::Camera> camera;
    std::unique_ptr<ui::OpenSong> openSong;
    ui::MainComponent content;
    std::unique_ptr<juce::FileChooser> chooser;
    std::unique_ptr<juce::DocumentWindow> settingsWindow;
    ui::DeviceSettingsPanel* settingsPanel = nullptr; // owned by settingsWindow

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
};
} // namespace overdub::app
