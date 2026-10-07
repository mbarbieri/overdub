#pragma once

#include "core/Error.h"
#include "ui/Commands.h"
#include "ui/Pages.h"
#include "ui/RecordPage.h"
#include "ui/SongSession.h"
#include "ui/TopBar.h"
#include "ui/UserMessages.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <string>
#include <vector>

// The main window's content (issue #27, issue #9 resolution): the top bar over the current mode's
// page (Record, or the Edit and Export placeholders), the Notice line, and Alerts. It translates
// keys and clicks into Commands and SongSession calls; the logic lives in the pure functions and
// in SongSession.
//
// Every 30 ms (and from tick() in tests) it polls the session, which finishes a stopped Pass,
// refreshes the top bar and the page, and presents queued messages: Notices at once, Alerts only
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
    void setSession (SongSession* session);
    SongSession* session() const { return songSession; }

    Mode mode() const { return currentMode; }

    // Runs a command as the matching key or button would.
    void perform (Command command);

    // Logs the Error once (warn for a Notice, error for an Alert) and presents it by its tier.
    void handleError (const core::Error& error);
    // Presents a Notice-tier code that was already logged where it arose (e.g. song.history_dropped).
    void showNotice (const std::string& code);

    // One poll: finish a stopped Pass, write a due song.json, refresh, present messages.
    void tick();
    // Shows the session's current state.
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
    PlaceholderPage editPage { "Edit mode: the timeline and mixer arrive in a later version." };
    PlaceholderPage exportPage { "Export mode arrives in a later version." };
    NoSongPage noSongPage;
    NoticeLine noticeLine;

private:
    void timerCallback() override { tick(); }

    void showMode (Mode mode);
    void importFiles (const std::vector<juce::File>& files);
    void post (UserMessage message);
    void presentMessages();
    void handleErrors (const std::vector<core::Error>& errors);
    void takeKeyboardFocus();

    SongSession* songSession = nullptr;
    Mode currentMode = Mode::record;
    UserMessageQueue messages;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
} // namespace overdub::ui
