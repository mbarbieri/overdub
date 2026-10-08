#pragma once

#include "core/Error.h"
#include "ui/Commands.h"
#include "ui/EditPage.h"
#include "ui/OpenSong.h"
#include "ui/Pages.h"
#include "ui/PluginWindows.h"
#include "ui/RecordPage.h"
#include "ui/TopBar.h"
#include "ui/UserMessages.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

// The main window's content (issue #27, issue #9 resolution): the top bar over the current mode's
// page (Record, Edit (issue #28), or the Export placeholder), the Notice line, and Alerts.
// In Edit mode S splits the selected Clip at the playhead and backspace deletes it, each one
// undo step through OpenSong::edit; in the other modes both keys do nothing. It translates
// keys and clicks into Commands and OpenSong calls; the logic lives in the pure functions and
// in OpenSong.
//
// Record mode (issue #32): the pre-roll picker sets the Song's pre-roll (no undo step); a click on a
// Pass row moves the playhead to the Pass's start, and on the overview to the Song sample there;
// ↺ Bring back is one undo step. While pre-roll counts down, the clock's status line does too.
//
// The camera (issue #35): the camera arm asks for camera access first (askCameraAccess), then arms
// the Song's camera (OpenSong::setCameraArmed); the picker chooses the camera. Neither is an undo
// step. The stage shows the live feed while the camera is Armed. A camera that cannot open, and a
// Pass's movie problems, are presented by their tier.
//
// Insert Chains (issues #33, #34) on Record rows, Mixer strips and the Master strip: "+ Insert"
// lists the AU effects and adds the one picked (OpenSong::addInsert), then opens its editor once
// it has loaded; bypass, remove and drag-to-reorder are one undo step each (song/InsertEdits.h).
// Clicking an Insert opens its plugin's editor window (ui/PluginWindows.h; a loading plugin's
// opens once it is ready, a missing one shows the "ui.plugin_missing" Notice). Windows follow the
// Song: one whose slot is removed (or undone away) closes; closing one schedules a song.json
// write that captures the plugin's state, outside history.
//
// M and S (Record rows, Edit headers, Mixer strips) and the Mixer's faders and pan are undo steps
// through OpenSong::edit; a Mixer drag in progress is heard through OpenSong::previewMix (issue #31).
//
// Every 30 ms (and from tick() in tests) it polls the open Song, which finishes a stopped Pass,
// refreshes the top bar and the page, feeds the Mixer's meters, and presents queued messages: Notices at once, Alerts only
// when no Pass runs (ADR 0006). Logs ui.mode_changed (debug), ui.notice_shown, ui.alert_shown and
// ui.alert_deferred (info), each with `code` and `tier` where it names a message.
namespace overdub::ui
{
class MainComponent final : public juce::Component, private juce::Timer
{
public:
    static constexpr int pollIntervalMs = 30;

    MainComponent();
    ~MainComponent() override;

    // The open Song, or null for none. Not owned; it must outlive its use here.
    void setOpenSong (OpenSong* song);
    OpenSong* currentSong() const { return openSong; }

    Mode mode() const { return currentMode; }

    // Runs a command as the matching key or button would.
    void perform (Command command);

    // Logs the Error once (warn for a Notice, error for an Alert) and presents it by its tier.
    void handleError (const core::Error& error);
    // Presents a Notice-tier code that was already logged where it arose (e.g. song.history_dropped).
    void showNotice (const std::string& code);

    // One poll: finish a stopped Pass, write a due song.json, refresh, present messages.
    void tick();
    // Shows the open Song's current state.
    void refresh();

    bool keyPressed (const juce::KeyPress& key) override;
    void resized() override;
    void paint (juce::Graphics& g) override;

    // Injection points (ADR 0005 UI amendment); defaults use native dialogs.
    // Asks for audio files to import and calls back with them (none when cancelled).
    std::function<void (std::function<void (std::vector<juce::File>)>)> chooseFilesToImport;
    // Shows an Alert: a modal dialog with this sentence.
    std::function<void (const juce::String& sentence)> showAlert;
    // "+ Insert" (issue #33): the AU effects to offer (AudioUnitLoader::findEffects
    // in the app; none by default), and how the user picks one: calls back with the index picked,
    // or none when dismissed (default: a pop-up menu by name).
    std::function<std::vector<plugin::AvailableAudioUnit>()> listInsertPlugins;
    std::function<void (const std::vector<plugin::AvailableAudioUnit>&, std::function<void (std::optional<std::size_t>)>)> chooseInsertPlugin;
    // Camera access before arming (issue #35): calls back with whether the app may use the camera
    // (default: capture::requestCameraAccess, the system prompt).
    std::function<void (std::function<void (bool granted)>)> askCameraAccess;
    // The File menu's New Song and Open, offered on the no-Song page.
    std::function<void()> onNewSong;
    std::function<void()> onOpenSong;

    // Children are public so tests can click them and check what shows.
    TopBar topBar;
    RecordPage recordPage;
    EditPage editPage;
    PlaceholderPage exportPage { "Export mode arrives in a later version." };
    NoSongPage noSongPage;
    NoticeLine noticeLine;
    // The plugin editor windows; tests set showOnDesktop false before opening any.
    PluginWindows pluginWindows;

private:
    void timerCallback() override { tick(); }

    void showMode (Mode mode);
    void jumpToPass (const song::Id& passId);
    void splitSelectedClip();
    void deleteSelectedClip();
    void applyEdit (core::Expected<song::Song> changed, const std::string& label);
    void armCamera (bool armed);
    void setMute (const song::Id& trackId, bool mute);
    void setSolo (const song::Id& trackId, bool solo);
    void importFiles (const std::vector<juce::File>& files);
    void chooseInsert (const song::Id& trackId);
    void openInsertEditor (const song::Id& slotId);
    void syncPluginWindows();
    InsertChainActions insertChainActions();
    void post (UserMessage message);
    void presentMessages();
    void handleErrors (const std::vector<core::Error>& errors);
    void takeKeyboardFocus();
    bool passRunning() const { return openSong != nullptr && openSong->isRecording(); }

    OpenSong* openSong = nullptr;
    Mode currentMode = Mode::record;
    UserMessageQueue messages;
    // A slot whose editor opens once its plugin is ready (just added, or clicked while loading).
    std::optional<song::Id> pendingEditorSlot;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
} // namespace overdub::ui
