#pragma once

#include "core/Error.h"
#include "ui/Commands.h"
#include "ui/EditPage.h"
#include "ui/OpenSong.h"
#include "ui/Pages.h"
#include "ui/RecordPage.h"
#include "ui/TopBar.h"
#include "ui/UserMessages.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
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

private:
    void timerCallback() override { tick(); }

    void showMode (Mode mode);
    void jumpToPass (const song::Id& passId);
    void splitSelectedClip();
    void deleteSelectedClip();
    void applyEdit (core::Expected<song::Song> changed, const std::string& label);
    void setMute (const song::Id& trackId, bool mute);
    void setSolo (const song::Id& trackId, bool solo);
    void importFiles (const std::vector<juce::File>& files);
    void post (UserMessage message);
    void presentMessages();
    void handleErrors (const std::vector<core::Error>& errors);
    void takeKeyboardFocus();
    bool passRunning() const { return openSong != nullptr && openSong->isRecording(); }

    OpenSong* openSong = nullptr;
    Mode currentMode = Mode::record;
    UserMessageQueue messages;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
} // namespace overdub::ui
