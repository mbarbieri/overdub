#include "ui/MainComponent.h"

#include "capture/AvCamera.h"
#include "core/Log.h"

#include "ui/Colours.h"
#include "ui/FileDrop.h"
#include "ui/Layout.h"

#include <cmath>
#include <memory>

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

// The App group passes: the Settings window is outside the dialog, and the Workspace refuses with a
// presented Notice what may not run.
bool actsUnderRunDialog (const Intent& running)
{
    if (const auto* song = std::get_if<SongIntent> (&running))
        return std::holds_alternative<intent::CancelRun> (*song);
    return std::holds_alternative<AppIntent> (running);
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

void showAlertWithNativeDialog (const juce::String& sentence)
{
    juce::AlertWindow::showAsync (juce::MessageBoxOptions::makeOptionsOk (juce::MessageBoxIconType::WarningIcon, "Overdub", sentence), nullptr);
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
        songContent->refreshVideo (nowHostNs, refreshNs, isVideoPreviewShown() ? &videoPreview->screen() : nullptr);
}

void MainComponent::setVideoPreviewWanted (bool wanted)
{
    if (wanted == videoPreviewWanted)
        return;
    videoPreviewWanted = wanted;
    core::logEvent (core::Level::info, "ui.video_preview_toggled", { { "shown", wanted } });
    syncVideoPreview();
}

void MainComponent::syncVideoPreview()
{
    const auto shown = videoPreviewWanted && songContent != nullptr && currentMode == Mode::edit;
    if (songContent != nullptr)
        songContent->editPage.setVideoPreviewShown (videoPreviewWanted);
    if (shown && videoPreview == nullptr)
    {
        videoPreview = std::make_unique<VideoPreviewWindow> (showVideoPreviewOnDesktop);
        // Closed from its own close button: the toggle goes off.
        videoPreview->onClose = [this] { run (WindowIntent { intent::SetVideoPreviewWanted { false } }); };
    }
    if (videoPreview != nullptr)
        videoPreview->setShown (shown);
}

void MainComponent::setOpenSong (OpenSong* song)
{
    // Destroyed first: its plugin windows close and its pending callbacks expire with it.
    songContent.reset();
    if (song != nullptr)
    {
        songContent = std::make_unique<SongView> (*song, *this);
        addAndMakeVisible (*songContent, getIndexOfChildComponent (&noticeLine));
    }
    resized();
    refresh();
}

IntentResult MainComponent::run (const Intent& intent, Origin origin)
{
    const auto op = std::string (opName (intent));
    const auto dragFrameChangingOnlyWhatIsHeard = op == intent::PreviewMix::op;
    if (! dragFrameChangingOnlyWhatIsHeard)
        core::logEvent (core::Level::debug, "ui.intent_run", { { "op", op }, { "origin", originName (origin) } });
    if (runShowing() && ! actsUnderRunDialog (intent))
        return quietError (core::Error { "ui.run_dialog_open", "a Separation or a conversion runs", { { "op", op } } });
    auto result = core::visitExhaustively (
        intent,
        core::Overloaded {
            [this] (const AppIntent& app) { return runApp (app); },
            [this] (const WindowIntent& windowIntent) { return runWindow (windowIntent); },
            [this, &op] (const SongIntent& songIntent) { return songContent != nullptr ? songContent->run (songIntent) : quietError (noSongOpen (op)); },
        });
    if (! dragFrameChangingOnlyWhatIsHeard)
        refresh();
    return result;
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
            [this] (const SetVideoPreviewWanted& preview) -> IntentResult { setVideoPreviewWanted (preview.wanted); return IntentDone {}; },
            // Alerts are native windows the app keeps no handle to yet (issue #171).
            [this] (const DismissAlerts&) { return quietError (notRouted (DismissAlerts::op)); },
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

void MainComponent::showNotice (const std::string& code)
{
    post (UserMessage { Tier::notice, code });
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
            noticeCode = message.code;
            if (const auto rate = conversionOfferedBy (message))
                noticeLine.show (sentence, juce::String::fromUTF8 (conversionActionText (*rate).c_str()), [this, toRate = *rate] { run (AppIntent { intent::ConvertSampleRate { toRate } }); });
            else
                noticeLine.show (sentence);
        }
        else
        {
            core::logEvent (core::Level::info, "ui.alert_shown", messageFields (message));
            if (showAlert)
                showAlert (sentence);
        }
    }
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
    // The conversion a Notice offers no longer applies once the Song is at the device's rate.
    if (noticeLine.hasAction() && (songContent == nullptr || ! songContent->currentSong().audioProblem().has_value()))
        noticeLine.dismiss();
    syncVideoPreview();
}

bool MainComponent::keyPressed (const juce::KeyPress& key)
{
    const auto modifiers = key.getModifiers();
    const auto intent = intentForKey (KeyChord {
        .character = (char32_t) key.getKeyCode(),
        .command = modifiers.isCommandDown(),
        .shift = modifiers.isShiftDown(),
    });
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
    runDialog.setBounds (getLocalBounds());
}

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (colours::background);
}
} // namespace overdub::ui
