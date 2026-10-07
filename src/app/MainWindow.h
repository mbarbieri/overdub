#pragma once

#include "app/LiveEngine.h"
#include "song/SongPackage.h"
#include "ui/DeviceSettingsPanel.h"
#include "ui/MainComponent.h"
#include "ui/SongSession.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

namespace overdub::app
{
// The one top-level window (issue #27). Closing it quits the app. It owns the live audio, the
// open Song (a ui::SongSession) and the content (ui::MainComponent), and runs what needs native
// dialogs: the File menu (New Song, Open, Duplicate), Undo / Redo in the Edit menu, and the
// settings window with the audio device picker (per machine).
//
// Opening another Song, changing the device and closing the window each finish a running Pass
// first (ui::SongSession's invariant). While a Pass runs, those menu items are disabled.
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
    void openSong();
    void duplicateSong();
    void showSettings();
    void applyDeviceSettings (const engine::DeviceSettings& settings);

    // Closes the open Song (finishing a running Pass) and shows this one.
    void showSong (std::unique_ptr<song::SongPackage> package, const std::vector<song::Notice>& notices);
    void openSongAt (const juce::File& location);
    void closeSong();
    bool passRunning() const;
    void updateTitle();

    juce::String appName;
    // Invariant: declared before `session`, so the device keeps running while a Pass finishes.
    std::unique_ptr<LiveEngine> live;
    std::unique_ptr<ui::SongSession> session;
    ui::MainComponent content;
    std::unique_ptr<juce::FileChooser> chooser;
    std::unique_ptr<juce::DocumentWindow> settingsWindow;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
};
} // namespace overdub::app
