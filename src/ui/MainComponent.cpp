#include "ui/MainComponent.h"

#include "song/ClipEdits.h"
#include "song/MixEdits.h"

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

// The fields of the ui.* events that name a message.
core::Fields messageFields (const UserMessage& message)
{
    return { { "code", message.code }, { "tier", tierName (message.tier) } };
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
        if (openSong != nullptr)
            openSong->setArmed (trackId, armed);
        refresh();
    };
    recordPage.onInput = [this] (const song::Id& trackId, std::vector<int> channels) {
        if (openSong != nullptr)
            openSong->setInputChannels (trackId, std::move (channels));
        refresh();
        takeKeyboardFocus();
    };
    recordPage.onMonitoring = [this] (const song::Id& trackId, bool on) {
        if (openSong != nullptr)
            openSong->setMonitoring (trackId, on);
        refresh();
    };
    recordPage.onMute = [this] (const song::Id& trackId, bool mute) { setMute (trackId, mute); };
    recordPage.onSolo = [this] (const song::Id& trackId, bool solo) { setSolo (trackId, solo); };
    recordPage.onImport = [this] {
        if (openSong == nullptr || openSong->isRecording() || ! chooseFilesToImport)
            return;
        chooseFilesToImport ([safeThis = juce::Component::SafePointer<MainComponent> (this)] (std::vector<juce::File> files) {
            if (safeThis != nullptr)
                safeThis->importFiles (files);
        });
    };
    editPage.onEdit = [this] (song::Song changed, std::string label) {
        applyEdit (std::move (changed), label);
    };
    editPage.onSeek = [this] (song::SampleCount sample) {
        if (openSong != nullptr)
            openSong->seek (sample);
        refresh();
    };
    editPage.onArm = [this] (const song::Id& trackId, bool armed) {
        if (openSong != nullptr)
            openSong->setArmed (trackId, armed);
        refresh();
    };
    editPage.onMute = [this] (const song::Id& trackId, bool mute) { setMute (trackId, mute); };
    editPage.onSolo = [this] (const song::Id& trackId, bool solo) { setSolo (trackId, solo); };
    editPage.onMixPreview = [this] (const song::Song& preview) {
        if (openSong != nullptr)
            openSong->previewMix (preview);
    };
    editPage.onSplit = [this] { perform (Command::splitClip); };
    editPage.onDelete = [this] { perform (Command::deleteClip); };
    editPage.timeline.peaksFor = [this] (const song::ClipSource& source) -> const song::Peaks& {
        static const song::Peaks none;
        return openSong != nullptr ? openSong->peaks (source) : none;
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

void MainComponent::setOpenSong (OpenSong* song)
{
    openSong = song;
    refresh();
}

void MainComponent::perform (Command command)
{
    if (const auto mode = modeFor (command))
    {
        showMode (*mode);
        return;
    }
    if (openSong == nullptr)
        return;

    switch (command)
    {
        case Command::record:
            if (auto started = openSong->record(); ! started.has_value())
                handleError (started.error());
            break;
        case Command::playOrStop: openSong->playOrStop(); break;
        case Command::rewind: openSong->rewind(); break;
        case Command::undo:
            if (openSong->package().canUndo())
                if (auto undone = openSong->undo(); ! undone.has_value())
                    handleError (undone.error());
            break;
        case Command::redo:
            if (openSong->package().canRedo())
                if (auto redone = openSong->redo(); ! redone.has_value())
                    handleError (redone.error());
            break;
        case Command::splitClip:
            if (currentMode == Mode::edit)
                splitSelectedClip();
            break;
        case Command::deleteClip:
            if (currentMode == Mode::edit)
                deleteSelectedClip();
            break;
        case Command::showRecord:
        case Command::showEdit:
        case Command::showExport:
            break;
    }
    refresh();
}

void MainComponent::splitSelectedClip()
{
    const auto selected = editPage.timeline.selectedClip();
    if (! selected.has_value() || passRunning())
        return;
    const auto rightHalf = song::makeId();
    auto split = song::splitClip (openSong->song(), *selected, openSong->position(), rightHalf);
    const auto wasSplit = split.has_value();
    applyEdit (std::move (split), song::splitClipLabel);
    if (wasSplit)
        editPage.timeline.selectClip (rightHalf);
}

void MainComponent::deleteSelectedClip()
{
    const auto selected = editPage.timeline.selectedClip();
    if (! selected.has_value() || passRunning())
        return;
    applyEdit (song::deleteClip (openSong->song(), *selected), song::deleteClipLabel);
}

void MainComponent::applyEdit (core::Expected<song::Song> changed, const std::string& label)
{
    if (openSong == nullptr || passRunning())
        return;
    if (! changed.has_value())
    {
        handleError (changed.error());
        return;
    }
    if (auto edited = openSong->edit (std::move (*changed), label); ! edited.has_value())
        handleError (edited.error());
    refresh();
}

void MainComponent::setMute (const song::Id& trackId, bool mute)
{
    if (openSong != nullptr)
        applyEdit (song::setTrackMute (openSong->song(), trackId, mute), song::muteLabel (openSong->song(), trackId, mute));
}

void MainComponent::setSolo (const song::Id& trackId, bool solo)
{
    if (openSong != nullptr)
        applyEdit (song::setTrackSolo (openSong->song(), trackId, solo), song::soloLabel (openSong->song(), trackId, solo));
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
    if (openSong == nullptr || files.empty())
        return;
    for (const auto& result : openSong->importFiles (files))
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
    if (message.tier == Tier::alert && passRunning())
        core::logEvent (core::Level::info, "ui.alert_deferred", messageFields (message));
    messages.post (std::move (message));
    presentMessages();
}

void MainComponent::presentMessages()
{
    for (const auto& message : messages.takeReady (passRunning()))
    {
        const auto sentence = juce::String::fromUTF8 (sentenceFor (message.code).c_str());
        if (message.tier == Tier::notice)
        {
            core::logEvent (core::Level::info, "ui.notice_shown", messageFields (message));
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

void MainComponent::tick()
{
    if (openSong != nullptr)
        handleErrors (openSong->poll());
    refresh();
    // Taken every poll, in every mode, so the meters show only what played since the last one.
    if (openSong != nullptr)
        editPage.mixer.showMeters (openSong->meterPeaks());
    presentMessages();
}

void MainComponent::refresh()
{
    TopBarState state;
    state.mode = currentMode;
    state.songOpen = openSong != nullptr;
    if (openSong != nullptr)
    {
        const auto& song = openSong->song();
        const auto& package = openSong->package();
        const auto recording = openSong->isRecording();
        state.playing = openSong->isPlaying();
        state.recording = recording;
        state.clock = clockText (openSong->position(), song.sampleRate);
        state.status = statusText (TransportStatus { state.playing, recording, openSong->recordingPassNumber() });
        state.undoText = undoButtonText (package.canUndo() ? package.undoLabel() : "");
        state.redoText = redoButtonText (package.canRedo() ? package.redoLabel() : "");
        state.canUndo = package.canUndo() && ! recording;
        state.canRedo = package.canRedo() && ! recording;

        recordPage.show (recordPageStateFor (
            song,
            openSong->audio().inputChannelNames,
            [this] (const song::Id& trackId) { return openSong->isMonitoring (trackId); },
            recording));
        if (currentMode == Mode::edit)
            editPage.show (TimelineState { song, openSong->position(), state.playing, ! recording });
    }
    topBar.show (state);

    const auto songOpen = openSong != nullptr;
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
