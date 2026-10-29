#include "ui/MainComponent.h"

#include "capture/AvCamera.h"
#include "core/Log.h"

#include "ui/Colours.h"
#include "ui/FileDrop.h"
#include "ui/Layout.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>
#include <vector>

namespace overdub::ui
{
namespace
{
void chooseFilesWithNativeDialog (std::function<void (std::vector<juce::File>)> whenChosen)
{
    auto chooser = std::make_shared<juce::FileChooser> ("Import audio files",
                                                        juce::File::getSpecialLocation (juce::File::userMusicDirectory),
                                                        importableAudioPattern());
    const auto flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::canSelectMultipleItems;
    chooser->launchAsync (flags, [chooser, done = std::move (whenChosen)] (const juce::FileChooser& finished) {
        std::vector<juce::File> files;
        for (const auto& file : finished.getResults())
            files.push_back (file);
        done (std::move (files));
    });
}

// A pop-up menu of the plugins by name ("AUDelay (Apple)"); none picked when dismissed.
void choosePluginFromMenu (const std::vector<plugin::AvailableAudioUnit>& plugins, std::function<void (std::optional<std::size_t>)> whenChosen)
{
    juce::PopupMenu menu;
    if (plugins.empty())
        menu.addItem (-1, "No AU effects installed", false);
    for (std::size_t i = 0; i < plugins.size(); ++i)
        menu.addItem ((int) i + 1, juce::String::fromUTF8 ((plugins[i].name + " (" + plugins[i].manufacturer + ")").c_str()));
    menu.showMenuAsync (juce::PopupMenu::Options(), [done = std::move (whenChosen)] (int picked) {
        done (picked > 0 ? std::optional<std::size_t> ((std::size_t) picked - 1) : std::nullopt);
    });
}

// A pop-up menu of the items; none picked when dismissed. JUCE menus have no tooltips, so a
// disabled item's tooltip shows dimmed at its right, where a shortcut would.
void chooseTrackMenuItemFromMenu (const std::vector<TrackMenuItem>& items, std::function<void (std::optional<std::size_t>)> whenChosen)
{
    juce::PopupMenu menu;
    for (std::size_t i = 0; i < items.size(); ++i)
    {
        juce::PopupMenu::Item item { juce::String::fromUTF8 (items[i].label.c_str()) };
        item.setID ((int) i + 1).setEnabled (items[i].enabled);
        item.shortcutKeyDescription = juce::String::fromUTF8 (items[i].tooltip.c_str());
        menu.addItem (std::move (item));
    }
    menu.showMenuAsync (juce::PopupMenu::Options(), [done = std::move (whenChosen)] (int picked) {
        done (picked > 0 ? std::optional<std::size_t> ((std::size_t) picked - 1) : std::nullopt);
    });
}

core::Error notRouted (std::string_view op)
{
    return core::Error { "ui.intent_not_routed", "this operation does not run through the window yet", { { "op", std::string (op) } } };
}

// The fields of the ui.* events that name a message.
core::Fields messageFields (const UserMessage& message)
{
    return { { "code", message.code }, { "tier", tierName (message.tier) } };
}

void chooseExportFileWithNativeDialog (juce::File current, std::function<void (std::optional<juce::File>)> whenChosen)
{
    auto chooser = std::make_shared<juce::FileChooser> ("Export the mix", current, "*." + current.getFileExtension().trimCharactersAtStart ("."));
    const auto flags = juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting;
    chooser->launchAsync (flags, [chooser, done = std::move (whenChosen)] (const juce::FileChooser& finished) {
        const auto chosen = finished.getResult();
        done (chosen == juce::File() ? std::nullopt : std::optional<juce::File> (chosen));
    });
}

// The Intent a Notice's action button runs and its label; none for a Notice without one.
std::optional<std::pair<Intent, std::string>> noticeAction (const UserMessage& message)
{
    if (const auto rate = conversionOfferedBy (message))
        return std::pair<Intent, std::string> { AppIntent { intent::ConvertSampleRate { *rate } }, conversionActionText (*rate) };
    if (message.code == exportFinishedCode)
        return std::pair<Intent, std::string> { SongIntent { intent::RevealExport {} }, revealActionText };
    return std::nullopt;
}

MainComponent::CloseAlert showAlertWithNativeDialog (const juce::String& sentence, std::function<void()> closed)
{
    const auto options = juce::MessageBoxOptions::makeOptionsOk (juce::MessageBoxIconType::WarningIcon, "Overdub", sentence);
    // Closing the ScopedMessageBox closes its window without calling back; closing it again, or
    // after the user did, does nothing.
    auto box = std::make_shared<juce::ScopedMessageBox> (juce::AlertWindow::showScopedAsync (options, [done = std::move (closed)] (int) { done(); }));
    return [box] { box->close(); };
}

void confirmDeleteWithNativeDialog (const juce::String& question, std::function<void (bool)> whenAnswered)
{
    const auto options = juce::MessageBoxOptions::makeOptionsOkCancel (juce::MessageBoxIconType::QuestionIcon, "Overdub", question, "Delete", "Cancel");
    juce::AlertWindow::showAsync (options, [done = std::move (whenAnswered)] (int result) { done (result == 1); });
}
} // namespace

MainComponent::MainComponent()
{
    chooseFilesToImport = chooseFilesWithNativeDialog;
    chooseInsertPlugin = choosePluginFromMenu;
    chooseTrackMenuItem = chooseTrackMenuItemFromMenu;
    showAlert = showAlertWithNativeDialog;
    confirmTrackDelete = confirmDeleteWithNativeDialog;
    askCameraAccess = capture::requestCameraAccess;
    chooseExportDestination = chooseExportFileWithNativeDialog;
    revealInFinder = [] (const juce::File& file) { file.revealToUser(); };

    topBar.onIntent = [this] (const Intent& intent) { run (intent); };
    runDialog.onCancel = [this] { run (SongIntent { intent::CancelRun {} }); };
    noSongPage.onNewSong = [this] {
        if (onNewSong)
            onNewSong();
    };
    noSongPage.onOpenSong = [this] {
        if (onOpenSong)
            onOpenSong();
    };

    addAndMakeVisible (topBar);
    addChildComponent (noSongPage);
    addChildComponent (noticeLine);
    addChildComponent (runDialog); // last: on top of everything

    setWantsKeyboardFocus (true);
    addMouseListener (this, true);
    setSize (1200, 800);
    refresh();
    startTimer (pollIntervalMs);
    vblank = juce::VBlankAttachment (this, [this] (double timestampSeconds) { onVBlank (timestampSeconds); });
}

MainComponent::~MainComponent()
{
    vblank = {};
    stopTimer();
    // Out of the member first: closing one must not call back into a vector being destroyed.
    auto closing = std::exchange (openAlerts, {});
}

void MainComponent::onVBlank (double timestampSeconds)
{
    // One refresh is the interval between vertical blanks (60 Hz until two have been seen).
    auto refreshNs = defaultRefreshNs;
    if (lastVBlankSeconds.has_value())
    {
        const auto interval = timestampSeconds - *lastVBlankSeconds;
        if (interval > 1.0 / 240.0 && interval < 1.0 / 20.0)
            refreshNs = (std::uint64_t) std::llround (interval * 1.0e9);
    }
    lastVBlankSeconds = timestampSeconds;
    refreshVideo (core::hostTimeNowNs(), refreshNs);
}

void MainComponent::refreshVideo (std::uint64_t nowHostNs, std::uint64_t refreshNs)
{
    if (songContent != nullptr)
        songContent->refreshVideo (nowHostNs, refreshNs);
}

void MainComponent::setOpenSong (OpenSong* song)
{
    // Destroyed first: its plugin windows close and its pending callbacks expire with it.
    songContent.reset();
    if (song != nullptr)
    {
        songContent = std::make_unique<SongView> (*song, *this);
        song->setTranscribing (currentMode == Mode::transcribe);
        addAndMakeVisible (*songContent, getIndexOfChildComponent (&noticeLine));
        // Over the top bar and the page, under the Notice line; the SongView owns it.
        addChildComponent (songContent->exportSheet, getIndexOfChildComponent (&noticeLine));
    }
    resized();
    refresh();
}

IntentResult MainComponent::run (const Intent& intent, Origin origin)
{
    const auto op = std::string (opName (intent));
    ++intentRunCount;
    const auto perFrame = runsPerFrame (intent);
    if (! perFrame)
        core::logEvent (core::Level::debug, "ui.intent_run", { { "op", op }, { "origin", originName (origin) } });
    // Invariant: a pending arrow-key burst is its own undo step before what any Intent does, so an
    // export, a copy or another Song change never misses it (issue #194). Per-frame Intents are
    // gesture phases, not steps.
    if (! perFrame && songContent != nullptr)
        songContent->commitKeyBursts();
    auto result = dispatch (intent, op);
    if (! perFrame)
        refresh();
    return result;
}

IntentResult MainComponent::dispatch (const Intent& intent, const std::string& op)
{
    if (auto refusal = ui::refusalOf (intent, running()))
        return refused (std::move (*refusal));
    return core::visitExhaustively (
        intent,
        core::Overloaded {
            [this] (const AppIntent& app) { return runApp (app); },
            [this] (const WindowIntent& windowIntent) { return runWindow (windowIntent); },
            [this, &op] (const SongIntent& songIntent) { return songContent != nullptr ? songContent->run (songIntent) : quietError (noSongOpen (op)); },
        });
}

Running MainComponent::running() const
{
    return Running {
        .song = songContent != nullptr ? std::optional (songContent->currentSong().activityKind()) : std::nullopt,
        .calibrating = calibrationRunning != nullptr && calibrationRunning(),
        .songHasNoPass = songContent != nullptr && songContent->currentSong().song().passes.empty(),
        .transposing = songContent != nullptr && songContent->currentSong().isTransposing(),
        .transcribing = currentMode == Mode::transcribe,
        .recordMode = currentMode == Mode::record,
    };
}

bool MainComponent::mayRun (std::string_view op)
{
    auto refusal = refusalOf (op);
    if (refusal.has_value())
        refused (std::move (*refusal));
    return ! refusal.has_value();
}

IntentResult MainComponent::refused (Refusal refusal)
{
    if (! refusal.presented)
        return quietError (std::move (refusal.error));
    handleError (refusal.error);
    return std::unexpected (std::move (refusal.error));
}

IntentResult MainComponent::runApp (const AppIntent& appIntent)
{
    if (! runAppIntent)
        return quietError (notRouted (core::operationName (appIntent)));
    return runAppIntent (appIntent);
}

IntentResult MainComponent::runWindow (const WindowIntent& windowIntent)
{
    using namespace intent;
    return core::visitExhaustively (
        windowIntent,
        core::Overloaded {
            [this] (const ShowMode& show) -> IntentResult { showMode (show.mode); return IntentDone {}; },
            [this] (const DismissAlerts&) { return dismissAlerts(); },
            [this] (const SetTranscribeColumnVisible& i) { return setTranscribeColumnVisible (i.visible); },
        });
}

IntentResult MainComponent::quietError (core::Error error)
{
    core::logError (error, core::Level::debug);
    return std::unexpected (std::move (error));
}

void MainComponent::showMode (Mode mode)
{
    if (mode != currentMode)
    {
        if (songContent != nullptr)
            songContent->releaseFocus();
        core::logEvent (core::Level::debug, "ui.mode_changed", { { "mode", modeName (mode) } });
    }
    currentMode = mode;
    // Transcribe mode hears the Transcribe Mix; Record and Edit the Song's own mix (issue #226).
    if (songContent != nullptr)
        songContent->currentSong().setTranscribing (mode == Mode::transcribe);
    refresh();
}

IntentResult MainComponent::setTranscribeColumnVisible (bool visible)
{
    if (visible != windowSettings.transcribeColumnVisible)
        core::logEvent (core::Level::debug, "ui.transcribe_column_visibility_changed", { { "visible", visible } });
    windowSettings.transcribeColumnVisible = visible;
    // The column shows either way: a settings file that cannot be written only forgets it.
    if (windowSettingsFile != juce::File())
        if (auto written = writeWindowSettings (windowSettingsFile, windowSettings); ! written.has_value())
            core::logError (written.error(), core::Level::warn);
    refresh();
    return IntentDone {};
}

void MainComponent::useWindowSettingsFile (const juce::File& file)
{
    windowSettingsFile = file;
    if (file == juce::File())
        return;
    auto read = readWindowSettings (file);
    if (! read.has_value())
    {
        core::logError (read.error(), core::Level::warn);
        return;
    }
    windowSettings = *read;
    core::logEvent (core::Level::debug, "ui.window_settings_read", { { "path", file.getFullPathName().toStdString() }, { "transcribe_column_visible", windowSettings.transcribeColumnVisible } });
    refresh();
}

void MainComponent::handleError (const core::Error& error)
{
    const auto message = userMessageFor (error.code, error.fields);
    core::logError (error, message.tier == Tier::notice ? core::Level::warn : core::Level::error);
    post (message);
}

void MainComponent::handleUnlessRefused (const core::Error& error)
{
    if (! isRefusal (error))
        return handleError (error);
    // The page's controls already show the gesture is unavailable: no Notice (issue #145).
    core::logError (error, core::Level::debug);
}

void MainComponent::handleUnlessRefused (const core::Expected<void>& result)
{
    if (! result.has_value())
        handleUnlessRefused (result.error());
}

void MainComponent::showNotice (const std::string& code, core::Fields fields)
{
    post (UserMessage { Tier::notice, code, std::move (fields) });
}

void MainComponent::post (UserMessage message)
{
    if (message.tier == Tier::alert && passRunning())
        core::logEvent (core::Level::info, "ui.alert_deferred", messageFields (message));
    messages.post (std::move (message));
    presentMessages();
}

void MainComponent::presentMessages()
{
    for (const auto& message : messages.takeReady (passRunning()))
    {
        const auto sentence = juce::String::fromUTF8 (sentenceFor (message.code, message.fields).c_str());
        if (message.tier == Tier::notice)
        {
            core::logEvent (core::Level::info, "ui.notice_shown", messageFields (message));
            notice = ShownNotice { message, ++noticesEverShown, std::nullopt };
            if (auto action = noticeAction (message))
            {
                notice->action = action->first;
                noticeLine.show (sentence, juce::String::fromUTF8 (action->second.c_str()), [this, intent = std::move (action->first)] { run (intent); });
            }
            else
                noticeLine.show (sentence);
        }
        else
            showAlertNow (message, sentence);
    }
}

void MainComponent::showAlertNow (const UserMessage& message, const juce::String& sentence)
{
    core::logEvent (core::Level::info, "ui.alert_shown", messageFields (message));
    const auto alertId = ++alertsEverShown;
    openAlerts.push_back (OpenAlert { alertId, message, nullptr });
    if (! showAlert)
        return;
    auto close = showAlert (sentence, [window = juce::Component::SafePointer<MainComponent> (this), alertId] {
        if (window != nullptr)
            window->alertClosedByUser (alertId);
    });
    // Looked up again: an injected showAlert may have called `closed` already.
    const auto open = std::ranges::find (openAlerts, alertId, &OpenAlert::id);
    if (open != openAlerts.end())
        open->close = std::move (close);
}

void MainComponent::alertClosedByUser (std::uint64_t alertId)
{
    const auto open = std::ranges::find (openAlerts, alertId, &OpenAlert::id);
    if (open == openAlerts.end())
        return; // dismiss_alerts closed it first
    // Out of the list before its handle goes, which may call back into this.
    auto closed = std::move (*open);
    openAlerts.erase (open);
}

IntentResult MainComponent::dismissAlerts()
{
    for (auto& alert : std::exchange (openAlerts, {}))
    {
        core::logEvent (core::Level::info, "ui.alert_dismissed", messageFields (alert.message));
        if (alert.close)
            alert.close();
    }
    return IntentDone {};
}

std::vector<UserMessage> MainComponent::alertsShown() const
{
    std::vector<UserMessage> shown;
    for (const auto& alert : openAlerts)
        shown.push_back (alert.message);
    return shown;
}

void MainComponent::tick (song::SongPackage::Clock::time_point now)
{
    if (songContent != nullptr)
        songContent->poll (now);
    refresh();
    // Taken every poll, in every mode, so the meters show only what played since the last one.
    if (songContent != nullptr)
        songContent->showMeters();
    presentMessages();
}

void MainComponent::refresh()
{
    TopBarState state;
    state.mode = currentMode;
    if (songContent != nullptr)
        songContent->refresh (state);
    state.controllingProcesses = controllingProcesses;
    topBar.show (state);

    noSongPage.setVisible (songContent == nullptr);
    if (songContent != nullptr)
        songContent->showRun (runDialog);
    runDialog.setVisible (runShowing());
    // The conversion a Notice offers no longer applies once the Song is at the device's rate, and
    // no file is revealed once its Song has closed.
    const auto offersConversion = notice.has_value() && conversionOfferedBy (notice->message).has_value();
    if (noticeLine.hasAction() && (songContent == nullptr || (offersConversion && ! songContent->currentSong().audioProblem().has_value())))
        noticeLine.dismiss();
}

KeyContext MainComponent::keyContext() const
{
    KeyContext context { .transcribeColumnVisible = windowSettings.transcribeColumnVisible };
    if (songContent == nullptr)
        return context;
    const auto& song = songContent->currentSong().song();
    context.selectedMarker = currentMode != Mode::record ? songContent->selectedMarker() : std::nullopt;
    context.loopToDelete = songContent->loopToDelete();
    context.looping = song.looping;
    for (const auto& loop : song.loops)
        context.loopIds.push_back (loop.id);
    context.loopStartPending = songContent->loopStartPending().has_value();
    context.practiceSpeed = songContent->currentSong().practiceSpeed();
    return context;
}

bool MainComponent::keyPressed (const juce::KeyPress& key)
{
    const auto modifiers = key.getModifiers();
    const auto intent = intentForKey (KeyChord {
                                          .character = (char32_t) key.getKeyCode(),
                                          .command = modifiers.isCommandDown(),
                                          .shift = modifiers.isShiftDown(),
                                          .option = modifiers.isAltDown(),
                                      },
                                      keyContext());
    // The run dialog is modal: it swallows every key while it shows.
    if (runShowing())
        return true;
    if (songContent != nullptr && songContent->handleKey (key, clock()))
        return true;
    if (! intent.has_value())
        return false;
    run (*intent);
    return true;
}

void MainComponent::mouseDown (const juce::MouseEvent& event)
{
    if (songContent != nullptr)
        songContent->releaseFocusOutside (event.originalComponent);
}

void MainComponent::resized()
{
    const auto layout = mainLayout (getLocalBounds());
    topBar.setBounds (layout.topBar);
    noSongPage.setBounds (layout.page);
    if (songContent != nullptr)
        songContent->setBounds (layout.page);
    noticeLine.setBounds (layout.noticeLine);
    if (songContent != nullptr)
        songContent->exportSheet.setBounds (getLocalBounds());
    runDialog.setBounds (getLocalBounds());
}

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (colours::background);
}

void reopenLastSongAtLaunch (MainComponent& content, bool skipLastSong)
{
    if (skipLastSong)
    {
        core::logEvent (core::Level::info, "ui.last_song_skipped", { { "reason", "shift_held" } });
        return;
    }
    content.run (AppIntent { intent::ReopenLastSong {} });
}
} // namespace overdub::ui
