#include "ui/MainComponent.h"

#include "capture/AvCamera.h"
#include "core/Log.h"
#include "export/MixRender.h"
#include "plugin/PluginKind.h"
#include "song/ClipEdits.h"
#include "song/InsertEdits.h"
#include "song/MixEdits.h"
#include "song/TakeEdits.h"
#include "song/TrackEdits.h"

#include "ui/Colours.h"
#include "ui/FileDrop.h"
#include "ui/Layout.h"
#include "ui/TransportText.h"

#include <algorithm>
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

    topBar.onCommand = [this] (Command command) { perform (command); };
    separationDialog.onCancel = [this] { cancelSeparation(); };
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
    recordPage.onPreRoll = [this] (song::PreRoll preRoll) {
        if (openSong != nullptr)
            openSong->setPreRoll (preRoll);
        refresh();
    };
    recordPage.onCameraArm = [this] (bool armed) { armCamera (armed); };
    recordPage.onCameraDevice = [this] (const std::string& cameraId) {
        if (openSong == nullptr)
            return;
        if (auto chosen = openSong->setCameraDevice (cameraId); ! chosen.has_value())
            handleError (chosen.error());
        refresh();
    };
    recordPage.onJumpToPass = [this] (const song::Id& passId) { jumpToPass (passId); };
    recordPage.onBringBack = [this] (const song::Id& passId) {
        if (openSong != nullptr)
            applyEdit (song::bringBackPass (openSong->song(), passId), song::bringBackPassLabel (openSong->song(), passId));
    };
    recordPage.onSeek = [this] (song::SampleCount sample) {
        if (openSong != nullptr)
            openSong->seek (sample);
        refresh();
    };
    recordPage.peaksFor = [this] (const song::ClipSource& source) -> const song::Peaks& {
        static const song::Peaks none;
        return openSong != nullptr ? openSong->peaks (source) : none;
    };
    const auto chainActions = insertChainActions();
    recordPage.insertActions = chainActions;
    editPage.mixer.insertActions = chainActions;
    editPage.mixer.insertStatusOf = [this] (const song::Id& slotId) { return openSong != nullptr ? openSong->insertStatus (slotId) : std::nullopt; };
    pluginWindows.onBypass = chainActions.bypass;
    pluginWindows.onClosed = [this] (const song::Id&) {
        if (openSong != nullptr)
            openSong->notePluginStateChanged();
    };
    recordPage.onAddTrack = [this] { addTrack(); };
    recordPage.onTrackMenu = [this] (const song::Id& trackId) { showTrackMenu (trackId); };
    recordPage.onImport = [this] {
        if (openSong == nullptr || ! openSong->canChangeTracks() || ! chooseFilesToImport)
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
    editPage.onToggleVideo = [this] { setVideoPreviewWanted (! videoPreviewWanted); };
    editPage.onDeleteTrack = [this] (const song::Id& trackId) { askToDeleteTrack (trackId); };
    editPage.onSplit = [this] { perform (Command::splitClip); };
    editPage.onDelete = [this] { perform (Command::deleteClip); };
    editPage.timeline.canImportFiles = [this] {
        return openSong != nullptr && openSong->canChangeTracks();
    };
    editPage.timeline.onFilesDropped = [this] (std::vector<juce::File> files) { importFiles (files); };
    editPage.timeline.peaksFor = [this] (const song::ClipSource& source) -> const song::Peaks& {
        static const song::Peaks none;
        return openSong != nullptr ? openSong->peaks (source) : none;
    };
    exportPage.onExport = [this] (exporting::ExportRequest request) { startExport (request); };
    exportPage.onChooseDestination = [this] (juce::File current) { chooseExportFile (current); };
    exportPage.onVideoTakeChosen = [this] (const song::Id& videoTakeId) {
        if (openSong != nullptr)
            openSong->setExportVideoTake (videoTakeId);
        refresh();
    };
    exportPage.onReveal = [this] (juce::File file) {
        if (revealInFinder)
            revealInFinder (file);
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
    addChildComponent (separationDialog); // last: on top of everything

    setWantsKeyboardFocus (true);
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
    if (openSong == nullptr)
        return;
    const auto previewShows = isVideoPreviewShown();
    const auto stageShows = currentMode == Mode::record && recordPage.showsStagePlayback();
    if (! previewShows && ! stageShows)
        return;
    const auto heard = video::heardSongSample (openSong->playbackClock(), nowHostNs, refreshNs);
    const auto frame = openSong->videoPlayback().frameAt (openSong->song(), heard);
    if (previewShows)
        videoPreview->screen().show (frame.choice, frame.sampleBuffer);
    if (stageShows)
        recordPage.stageVideo.show (frame.choice, frame.sampleBuffer);
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
    const auto shown = videoPreviewWanted && openSong != nullptr && currentMode == Mode::edit;
    editPage.setVideoPreviewShown (videoPreviewWanted);
    if (shown && videoPreview == nullptr)
    {
        videoPreview = std::make_unique<VideoPreviewWindow> (showVideoPreviewOnDesktop);
        // Closed from its own close button: the toggle goes off.
        videoPreview->onClose = [this] { setVideoPreviewWanted (false); };
    }
    if (videoPreview != nullptr)
        videoPreview->setShown (shown);
}

void MainComponent::setOpenSong (OpenSong* song)
{
    // Every window edits a plugin of the Song that was open.
    pluginWindows.closeAll();
    pendingEditorSlot.reset();
    openSong = song;
    if (openSong != nullptr)
        exportPage.setDestination (exporting::defaultExportFile (exportFolder, openSong->package().location().getFileNameWithoutExtension().toStdString(), exportPage.format()));
    refresh();
}

void MainComponent::startExport (const exporting::ExportRequest& request)
{
    if (openSong == nullptr)
        return;
    if (auto started = openSong->startExport (request); ! started.has_value())
        handleError (started.error());
    refresh();
}

void MainComponent::chooseExportFile (const juce::File& current)
{
    if (! chooseExportDestination)
        return;
    chooseExportDestination (current, [safeThis = juce::Component::SafePointer<MainComponent> (this)] (std::optional<juce::File> chosen) {
        if (safeThis == nullptr || ! chosen.has_value())
            return;
        safeThis->exportPage.setDestination (*chosen);
    });
}

void MainComponent::perform (Command command)
{
    // The Separation dialog is modal: nothing acts while it shows.
    if (separating())
        return;
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
        case Command::nudgeBack:
            if (currentMode == Mode::edit)
                nudgePlayhead (-nudgeSeconds);
            break;
        case Command::nudgeForward:
            if (currentMode == Mode::edit)
                nudgePlayhead (nudgeSeconds);
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

// OpenSong::seek clamps at the Song's start and ignores seeks while a Pass runs.
void MainComponent::nudgePlayhead (int seconds)
{
    const auto offset = (song::SampleCount) seconds * openSong->song().sampleRate;
    openSong->seek (openSong->position() + offset);
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

void MainComponent::armCamera (bool armed)
{
    if (openSong == nullptr || passRunning())
        return;
    const auto apply = [this] (bool arm) {
        if (openSong == nullptr)
            return;
        if (auto changed = openSong->setCameraArmed (arm); ! changed.has_value())
            handleError (changed.error());
        refresh();
    };
    if (! armed || ! askCameraAccess)
        return apply (armed);

    askCameraAccess ([safeThis = juce::Component::SafePointer<MainComponent> (this), apply] (bool granted) {
        if (safeThis == nullptr)
            return;
        if (granted)
            return apply (true);
        safeThis->handleError (core::Error { "capture.camera_access_denied", "the app may not use the camera", {} });
        safeThis->refresh();
    });
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

void MainComponent::jumpToPass (const song::Id& passId)
{
    if (openSong == nullptr)
        return;
    const auto& passes = openSong->song().passes;
    const auto pass = std::ranges::find (passes, passId, &song::Pass::id);
    if (! OVERDUB_CHECK (pass != passes.end(), "ui.pass_row_unknown", { "pass_id", passId }))
        return;
    openSong->seek (pass->startSample);
    refresh();
}

void MainComponent::showMode (Mode mode)
{
    if (mode != currentMode)
        core::logEvent (core::Level::debug, "ui.mode_changed", { { "mode", modeName (mode) } });
    currentMode = mode;
    refresh();
}

void MainComponent::addTrack()
{
    if (openSong == nullptr || passRunning())
        return;
    // Ready to record: Armed on the device's first input, or on none when it has no inputs.
    auto inputChannels = openSong->audio().inputChannelNames.empty() ? std::vector<int> {} : std::vector<int> { 0 };
    auto added = song::addTrack (openSong->song(), std::move (inputChannels));
    core::logEvent (core::Level::info, "ui.track_added", { { "track_id", added.trackId }, { "input_channels", added.song.tracks.back().inputChannels } });
    applyEdit (std::move (added.song), added.label);
    takeKeyboardFocus();
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

void MainComponent::askToDeleteTrack (const song::Id& trackId)
{
    if (openSong == nullptr || ! openSong->canChangeTracks() || ! confirmTrackDelete)
        return;
    const auto* track = song::findTrack (openSong->song(), trackId);
    if (track == nullptr)
        return;
    const auto question = juce::String::fromUTF8 (("Delete Track \"" + track->name + "\"? Its Clips and effects will be removed. You can undo this.").c_str());
    confirmTrackDelete (question, [safeThis = juce::Component::SafePointer<MainComponent> (this), trackId] (bool confirmed) {
        if (safeThis != nullptr && confirmed)
            safeThis->deleteTrack (trackId);
    });
}

void MainComponent::deleteTrack (const song::Id& trackId)
{
    // The answer comes later: by then the Song may be locked or the Track gone (undo).
    if (openSong == nullptr || ! openSong->canChangeTracks())
        return;
    auto deleted = song::deleteTrack (openSong->song(), trackId);
    if (! deleted.has_value())
        return;
    applyEdit (std::move (deleted->song), deleted->label);
    if (song::findTrack (openSong->song(), trackId) != nullptr)
        return; // the edit failed, and applyEdit reported it
    // Monitoring is transient: a deleted Track stops, and undo brings it back not Monitoring.
    openSong->setMonitoring (trackId, false);
    core::logEvent (core::Level::info, "ui.track_deleted", { { "track_id", trackId }, { "label", deleted->label } });
    takeKeyboardFocus();
}

void MainComponent::showTrackMenu (const song::Id& trackId)
{
    if (openSong == nullptr || separating() || ! chooseTrackMenuItem)
        return;
    auto items = trackMenuFor (openSong->song(), trackId, TrackMenuState { .passRunning = openSong->isRecording(), .exportRunning = openSong->isExporting() });
    chooseTrackMenuItem (items, [safeThis = juce::Component::SafePointer<MainComponent> (this), trackId, items] (std::optional<std::size_t> picked) {
        if (safeThis == nullptr || ! picked.has_value() || *picked >= items.size() || ! items[*picked].enabled)
            return;
        switch (items[*picked].action)
        {
            case TrackMenuAction::separate: safeThis->startSeparation (trackId); break;
        }
    });
}

void MainComponent::startSeparation (const song::Id& trackId)
{
    if (openSong == nullptr)
        return;
    if (auto started = openSong->startSeparation (trackId); ! started.has_value())
        handleError (started.error());
    refresh();
}

void MainComponent::cancelSeparation()
{
    if (openSong == nullptr)
        return;
    openSong->cancelSeparation();
    refresh();
    takeKeyboardFocus();
}

void MainComponent::chooseInsert (const song::Id& trackId)
{
    if (openSong == nullptr || openSong->isRecording() || ! chooseInsertPlugin)
        return;
    auto plugins = listInsertPlugins ? listInsertPlugins() : std::vector<plugin::AvailableAudioUnit> {};
    chooseInsertPlugin (plugins, [safeThis = juce::Component::SafePointer<MainComponent> (this), trackId, plugins] (std::optional<std::size_t> picked) {
        if (safeThis == nullptr || ! picked.has_value() || *picked >= plugins.size() || safeThis->openSong == nullptr)
            return;
        auto added = safeThis->openSong->addInsert (trackId, plugins[*picked]);
        if (! added.has_value())
            safeThis->handleError (added.error());
        else if (! added->empty())
            safeThis->pendingEditorSlot = *added; // its editor opens once it has loaded
        safeThis->refresh();
    });
}

InsertChainActions MainComponent::insertChainActions()
{
    InsertChainActions actions;
    actions.add = [this] (const song::Id& trackId) { chooseInsert (trackId); };
    actions.open = [this] (const song::Id& slotId) { openInsertEditor (slotId); };
    actions.bypass = [this] (const song::Id& slotId, bool bypassed) {
        if (openSong != nullptr)
            applyEdit (song::setInsertBypassed (openSong->song(), slotId, bypassed), song::bypassInsertLabel (openSong->song(), slotId, bypassed));
    };
    actions.remove = [this] (const song::Id& slotId) {
        if (openSong != nullptr)
            applyEdit (song::removeInsert (openSong->song(), slotId), song::removeInsertLabel (openSong->song(), slotId));
    };
    actions.move = [this] (const song::Id& slotId, std::size_t toIndex) {
        if (openSong != nullptr)
            applyEdit (song::moveInsert (openSong->song(), slotId, toIndex), song::moveInsertLabel (openSong->song(), slotId));
    };
    return actions;
}

void MainComponent::openInsertEditor (const song::Id& slotId)
{
    if (openSong == nullptr)
        return;
    const auto status = openSong->insertStatus (slotId);
    if (status == plugin::InsertStatus::failed)
    {
        showNotice ("ui.plugin_missing");
        return;
    }
    if (status == plugin::InsertStatus::crashed)
    {
        showNotice (openSong->insertHost().crashedForGood (slotId) ? "ui.plugin_crashed_repeatedly" : "ui.plugin_crashed");
        return;
    }
    // Loading: it opens when ready (syncPluginWindows).
    pendingEditorSlot = slotId;
    syncPluginWindows();
}

void MainComponent::syncPluginWindows()
{
    if (openSong == nullptr)
        return;
    const auto editable = ! openSong->isRecording();
    const auto stateOf = [this, editable] (const song::Id& slotId) { return pluginWindowStateFor (openSong->song(), slotId, {}, editable); };
    pluginWindows.sync (stateOf, [this] (const song::Id& slotId) { return openSong->insertHost().processor (slotId); });

    if (! pendingEditorSlot.has_value())
        return;
    const auto slotId = *pendingEditorSlot;
    const auto status = openSong->insertStatus (slotId);
    if (status == plugin::InsertStatus::loading)
        return;
    pendingEditorSlot.reset();
    if (status != plugin::InsertStatus::ready)
        return; // gone from the Song, or failed to load (logged where it failed)
    auto processor = openSong->insertHost().sharedProcessor (slotId);
    if (const auto state = pluginWindowStateFor (openSong->song(), slotId, plugin::hostingBadge (*processor), editable))
        pluginWindows.open (std::move (processor), *state);
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

void MainComponent::tick (song::SongPackage::Clock::time_point now)
{
    if (openSong != nullptr)
        handleErrors (openSong->poll (now));
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
        const auto saving = openSong->isSavingPass();
        state.recording = recording && ! saving; // the top bar reads REC only while capturing
        const auto recordPoint = openSong->preRollRecordPoint();
        const auto position = openSong->position();
        state.clock = clockText (position, song.sampleRate);
        state.status = statusText (TransportStatus {
            .playing = state.playing,
            .recording = state.recording,
            .saving = saving,
            .passNumber = openSong->recordingPassNumber(),
            .preRollRemaining = recordPoint.has_value() ? std::optional<song::SampleCount> (*recordPoint - position) : std::nullopt,
            .sampleRate = song.sampleRate,
        });
        state.undoText = undoButtonText (package.canUndo() ? package.undoLabel() : "");
        state.redoText = redoButtonText (package.canRedo() ? package.redoLabel() : "");
        state.canUndo = package.canUndo() && ! recording;
        state.canRedo = package.canRedo() && ! recording;

        recordPage.show (recordPageStateFor (
            song,
            openSong->audio().inputChannelNames,
            [this] (const song::Id& trackId) { return openSong->isMonitoring (trackId); },
            recording,
            [this] (const song::Id& slotId) { return openSong->insertStatus (slotId); }));
        recordPage.showCamera (cameraControlsStateFor (song, openSong->cameraDevices(), recording), openSong->cameraPreviewLayer());
        recordPage.showStagePlayback (! stageShowsLiveFeed (song.cameraArmed, recording, state.playing));
        if (currentMode == Mode::record)
            recordPage.showSong (song, RecordTransport { position, openSong->runningPassStart(), recordPoint }, ! recording);
        if (currentMode == Mode::edit)
            editPage.show (TimelineState { song, openSong->position(), state.playing, ! recording, openSong->canChangeTracks(), openSong->videoPlayback().spans (song) });
        const auto exportPhase = openSong->exportPhase();
        exportPage.show (ExportPageState {
            .clipsEnd = exporting::lastClipEnd (song),
            .sampleRate = song.sampleRate,
            .waitingForAudio = exportPhase == ExportPhase::waitingForAudio,
            .rendering = exportPhase == ExportPhase::rendering,
            .progress = openSong->exportProgress(),
            .finishedFile = openSong->lastExportedFile(),
            .videoTakes = openSong->videoPlayback().spans (song),
            .storedVideoTakeId = song.exportVideoTakeId,
        });
    }
    topBar.show (state);
    syncPluginWindows();

    const auto songOpen = openSong != nullptr;
    noSongPage.setVisible (! songOpen);
    recordPage.setVisible (songOpen && currentMode == Mode::record);
    editPage.setVisible (songOpen && currentMode == Mode::edit);
    exportPage.setVisible (songOpen && currentMode == Mode::exportSong);
    if (separating())
        separationDialog.show (openSong->separationSourceName(), openSong->separationProgress());
    separationDialog.setVisible (separating());
    syncVideoPreview();
}

bool MainComponent::keyPressed (const juce::KeyPress& key)
{
    const auto modifiers = key.getModifiers();
    const auto command = commandForKey (KeyChord {
        .character = (char32_t) key.getKeyCode(),
        .command = modifiers.isCommandDown(),
        .shift = modifiers.isShiftDown(),
    });
    // The Separation dialog is modal: it swallows every key while it shows.
    if (separating())
        return true;
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
    separationDialog.setBounds (getLocalBounds());
}

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (colours::background);
}
} // namespace overdub::ui
