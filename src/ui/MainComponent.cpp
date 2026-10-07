#include "ui/MainComponent.h"

#include "ui/Colours.h"
#include "ui/Layout.h"
#include "ui/TransportText.h"

#include <memory>

namespace overdub::ui
{
namespace
{
void chooseFilesWithNativeDialog (std::function<void (std::vector<juce::File>)> whenChosen)
{
    auto chooser = std::make_shared<juce::FileChooser> ("Import audio files",
                                                        juce::File::getSpecialLocation (juce::File::userMusicDirectory),
                                                        "*.wav;*.aif;*.aiff;*.aifc;*.caf;*.m4a;*.mp3;*.flac");
    const auto flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::canSelectMultipleItems;
    chooser->launchAsync (flags, [chooser, done = std::move (whenChosen)] (const juce::FileChooser& finished) {
        std::vector<juce::File> files;
        for (const auto& file : finished.getResults())
            files.push_back (file);
        done (std::move (files));
    });
}

void showAlertWithNativeDialog (const juce::String& sentence)
{
    juce::AlertWindow::showAsync (juce::MessageBoxOptions::makeOptionsOk (juce::MessageBoxIconType::WarningIcon, "Overdub", sentence), nullptr);
}
} // namespace

MainComponent::MainComponent()
{
    chooseFilesToImport = chooseFilesWithNativeDialog;
    showAlert = showAlertWithNativeDialog;

    topBar.onCommand = [this] (Command command) { perform (command); };
    recordPage.onArm = [this] (const song::Id& trackId, bool armed) {
        if (songSession != nullptr)
            songSession->setArmed (trackId, armed);
        refresh();
    };
    recordPage.onInput = [this] (const song::Id& trackId, std::vector<int> channels) {
        if (songSession != nullptr)
            songSession->setInputChannels (trackId, std::move (channels));
        refresh();
        takeKeyboardFocus();
    };
    recordPage.onMonitoring = [this] (const song::Id& trackId, bool on) {
        if (songSession != nullptr)
            songSession->setMonitoring (trackId, on);
        refresh();
    };
    recordPage.onImport = [this] {
        if (songSession == nullptr || songSession->isRecording() || ! chooseFilesToImport)
            return;
        chooseFilesToImport ([safeThis = juce::Component::SafePointer<MainComponent> (this)] (std::vector<juce::File> files) {
            if (safeThis != nullptr)
                safeThis->importFiles (files);
        });
    };
    noSongPage.onNewSong = [this] {
        if (onNewSong)
            onNewSong();
    };
    noSongPage.onOpenSong = [this] {
        if (onOpenSong)
            onOpenSong();
    };

    addAndMakeVisible (topBar);
    addChildComponent (recordPage);
    addChildComponent (editPage);
    addChildComponent (exportPage);
    addChildComponent (noSongPage);
    addChildComponent (noticeLine);

    setWantsKeyboardFocus (true);
    setSize (1200, 800);
    refresh();
    startTimer (pollIntervalMs);
}

MainComponent::~MainComponent()
{
    stopTimer();
}

void MainComponent::setSession (SongSession* session)
{
    songSession = session;
    refresh();
}

void MainComponent::perform (Command command)
{
    if (const auto mode = modeFor (command))
    {
        showMode (*mode);
        return;
    }
    if (songSession == nullptr)
        return;

    switch (command)
    {
        case Command::record:
            if (auto started = songSession->record(); ! started.has_value())
                handleError (started.error());
            break;
        case Command::playOrStop: songSession->playOrStop(); break;
        case Command::stop: songSession->stop(); break;
        case Command::rewind: songSession->rewind(); break;
        case Command::undo:
            if (songSession->package().canUndo())
                if (auto undone = songSession->undo(); ! undone.has_value())
                    handleError (undone.error());
            break;
        case Command::redo:
            if (songSession->package().canRedo())
                if (auto redone = songSession->redo(); ! redone.has_value())
                    handleError (redone.error());
            break;
        case Command::showRecord:
        case Command::showEdit:
        case Command::showExport:
            break;
    }
    refresh();
}

void MainComponent::showMode (Mode mode)
{
    if (mode != currentMode)
        core::logEvent (core::Level::debug, "ui.mode_changed", { { "mode", modeName (mode) } });
    currentMode = mode;
    refresh();
}

void MainComponent::importFiles (const std::vector<juce::File>& files)
{
    if (songSession == nullptr || files.empty())
        return;
    for (const auto& result : songSession->importFiles (files))
        if (! result.has_value())
            handleError (result.error());
    refresh();
    takeKeyboardFocus();
}

void MainComponent::handleError (const core::Error& error)
{
    const auto message = userMessageFor (error.code);
    core::logError (error, message.tier == Tier::notice ? core::Level::warn : core::Level::error);
    post (message);
}

void MainComponent::showNotice (const std::string& code)
{
    post (UserMessage { Tier::notice, code });
}

void MainComponent::handleErrors (const std::vector<core::Error>& errors)
{
    for (const auto& error : errors)
        handleError (error);
}

void MainComponent::post (UserMessage message)
{
    const auto passRunning = songSession != nullptr && songSession->isRecording();
    if (message.tier == Tier::alert && passRunning)
        core::logEvent (core::Level::info, "ui.alert_deferred", { { "code", message.code }, { "tier", tierName (message.tier) } });
    messages.post (std::move (message));
    presentMessages();
}

void MainComponent::presentMessages()
{
    const auto passRunning = songSession != nullptr && songSession->isRecording();
    for (const auto& message : messages.takeReady (passRunning))
    {
        const auto sentence = juce::String::fromUTF8 (sentenceFor (message.code).c_str());
        if (message.tier == Tier::notice)
        {
            core::logEvent (core::Level::info, "ui.notice_shown", { { "code", message.code }, { "tier", tierName (message.tier) } });
            noticeLine.show (sentence);
        }
        else
        {
            core::logEvent (core::Level::info, "ui.alert_shown", { { "code", message.code }, { "tier", tierName (message.tier) } });
            if (showAlert)
                showAlert (sentence);
        }
    }
}

void MainComponent::tick()
{
    if (songSession != nullptr)
        handleErrors (songSession->poll());
    refresh();
    presentMessages();
}

void MainComponent::refresh()
{
    TopBarState state;
    state.mode = currentMode;
    state.songOpen = songSession != nullptr;
    if (songSession != nullptr)
    {
        const auto& song = songSession->song();
        const auto& package = songSession->package();
        const auto recording = songSession->isRecording();
        state.playing = songSession->isPlaying();
        state.recording = recording;
        state.clock = clockText (songSession->position(), song.sampleRate);
        state.status = statusText (TransportStatus { state.playing, recording, songSession->recordingPassNumber() });
        state.undoText = undoButtonText (package.canUndo() ? package.undoLabel() : "");
        state.redoText = redoButtonText (package.canRedo() ? package.redoLabel() : "");
        state.canUndo = package.canUndo() && ! recording;
        state.canRedo = package.canRedo() && ! recording;

        recordPage.show (recordPageStateFor (
            song,
            songSession->audio().inputChannelNames,
            [this] (const song::Id& trackId) { return songSession->isMonitoring (trackId); },
            recording));
    }
    topBar.show (state);

    const auto songOpen = songSession != nullptr;
    noSongPage.setVisible (! songOpen);
    recordPage.setVisible (songOpen && currentMode == Mode::record);
    editPage.setVisible (songOpen && currentMode == Mode::edit);
    exportPage.setVisible (songOpen && currentMode == Mode::exportSong);
}

bool MainComponent::keyPressed (const juce::KeyPress& key)
{
    const auto modifiers = key.getModifiers();
    const auto command = commandForKey (KeyChord {
        .character = (char32_t) key.getKeyCode(),
        .command = modifiers.isCommandDown(),
        .shift = modifiers.isShiftDown(),
    });
    if (! command.has_value())
        return false;
    perform (*command);
    return true;
}

void MainComponent::takeKeyboardFocus()
{
    if (isShowing())
        grabKeyboardFocus();
}

void MainComponent::resized()
{
    const auto layout = mainLayout (getLocalBounds());
    topBar.setBounds (layout.topBar);
    for (auto* page : std::initializer_list<juce::Component*> { &recordPage, &editPage, &exportPage, &noSongPage })
        page->setBounds (layout.page);
    noticeLine.setBounds (layout.noticeLine);
}

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (colours::background);
}
} // namespace overdub::ui
