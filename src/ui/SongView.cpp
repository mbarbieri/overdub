#include "ui/SongView.h"

#include "core/Log.h"
#include "export/MixRender.h"
#include "plugin/PluginKind.h"
#include "song/ClipEdits.h"
#include "song/InsertEdits.h"
#include "song/MixEdits.h"
#include "song/TakeEdits.h"
#include "song/TrackEdits.h"
#include "song/VideoTakeEdits.h"

#include "ui/MainComponent.h"
#include "ui/TransportText.h"
#include "ui/VideoScreen.h"

#include <algorithm>

namespace overdub::ui
{
SongView::SongView (OpenSong& song, MainComponent& mainWindow)
    : openSong (song),
      window (mainWindow),
      trackActions (makeTrackActions()),
      recordPage (trackActions),
      editPage (trackActions),
      pluginWindows (mainWindow.showPluginWindowsOnDesktop)
{
    wirePages();
    exportPage.setDestination (exporting::defaultExportFile (window.exportFolder, openSong.package().location().getFileNameWithoutExtension().toStdString(), exportPage.format()));

    addChildComponent (recordPage);
    addChildComponent (editPage);
    addChildComponent (exportPage);
}

SongView::~SongView() = default;

TrackActions SongView::makeTrackActions()
{
    TrackActions actions;
    actions.arm = [this] (const song::Id& trackId, bool armed) {
        window.handleUnlessRefused (openSong.setArmed (trackId, armed));
        window.refresh();
    };
    actions.mute = [this] (const song::Id& trackId, bool mute) { setMute (trackId, mute); };
    actions.solo = [this] (const song::Id& trackId, bool solo) { setSolo (trackId, solo); };
    actions.moveTrack = [this] (const song::Id& trackId, std::size_t toIndex) { moveTrack (trackId, toIndex); };
    actions.seek = [this] (song::SampleCount sample) {
        window.handleUnlessRefused (openSong.seek (sample));
        window.refresh();
    };
    actions.previewMix = [this] (const song::Song& preview) { window.handleUnlessRefused (openSong.previewMix (preview)); };
    actions.peaksFor = [this] (const song::ClipSource& source) -> const song::Peaks& { return openSong.peaks (source); };
    return actions;
}

InsertChainActions SongView::makeInsertChainActions()
{
    InsertChainActions actions;
    actions.add = [this] (const song::Id& trackId) { chooseInsert (trackId); };
    actions.open = [this] (const song::Id& slotId) { openInsertEditor (slotId); };
    actions.bypass = [this] (const song::Id& slotId, bool bypassed) {
        applyEdit (song::setInsertBypassed (openSong.song(), slotId, bypassed), song::bypassInsertLabel (openSong.song(), slotId, bypassed));
    };
    actions.remove = [this] (const song::Id& slotId) {
        applyEdit (song::removeInsert (openSong.song(), slotId), song::removeInsertLabel (openSong.song(), slotId));
    };
    actions.move = [this] (const song::Id& slotId, std::size_t toIndex) {
        applyEdit (song::moveInsert (openSong.song(), slotId, toIndex), song::moveInsertLabel (openSong.song(), slotId));
    };
    return actions;
}

void SongView::wirePages()
{
    recordPage.onInput = [this] (const song::Id& trackId, std::vector<int> channels) {
        window.handleUnlessRefused (openSong.setInputChannels (trackId, std::move (channels)));
        window.refresh();
        takeKeyboardFocus();
    };
    recordPage.onMonitoring = [this] (const song::Id& trackId, bool on) {
        window.handleUnlessRefused (openSong.setMonitoring (trackId, on));
        window.refresh();
    };
    recordPage.onPreRoll = [this] (song::PreRoll preRoll) {
        window.handleUnlessRefused (openSong.setPreRoll (preRoll));
        window.refresh();
    };
    recordPage.onCameraArm = [this] (bool armed) { armCamera (armed); };
    recordPage.onCameraDevice = [this] (const std::string& cameraId) {
        window.handleUnlessRefused (openSong.setCameraDevice (cameraId));
        window.refresh();
    };
    recordPage.onJumpToPass = [this] (const song::Id& passId) { jumpToPass (passId); };
    recordPage.onBringBack = [this] (const song::Id& passId) {
        applyEdit (song::bringBackPass (openSong.song(), passId), song::bringBackPassLabel (openSong.song(), passId));
    };
    recordPage.mix.onEdit = [this] (song::Song changed, std::string label) { applyEdit (std::move (changed), label); };
    recordPage.onAddTrack = [this] { addTrack(); };
    recordPage.onTrackMenu = [this] (const song::Id& trackId) { showTrackMenu (trackId); };
    recordPage.onImport = [this] { askToImport(); };

    const auto chainActions = makeInsertChainActions();
    recordPage.insertActions = chainActions;
    editPage.mixer.insertActions = chainActions;
    editPage.mixer.insertStatusOf = [this] (const song::Id& slotId) { return openSong.insertStatus (slotId); };
    pluginWindows.onBypass = chainActions.bypass;
    pluginWindows.onClosed = [this] (const song::Id&) { openSong.notePluginStateChanged(); };

    editPage.onEdit = [this] (song::Song changed, std::string label) { applyEdit (std::move (changed), label); };
    editPage.onToggleVideo = [this] { window.setVideoPreviewWanted (! window.isVideoPreviewWanted()); };
    editPage.onDeleteTrack = [this] (const song::Id& trackId) { askToDeleteTrack (trackId); };
    editPage.onSplit = [this] { window.perform (Command::splitClip); };
    editPage.onDelete = [this] { window.perform (Command::deleteSelection); };
    editPage.timeline.canImportFiles = [this] { return openSong.canChangeTracks(); };
    editPage.timeline.onFilesDropped = [this] (std::vector<juce::File> files) { importFiles (files); };

    exportPage.onExport = [this] (exporting::ExportRequest request) { startExport (request); };
    exportPage.onChooseDestination = [this] (juce::File current) { chooseExportFile (current); };
    exportPage.onVideoTakeChosen = [this] (const song::Id& videoTakeId) {
        openSong.setExportVideoTake (videoTakeId);
        window.refresh();
    };
    exportPage.onReveal = [this] (juce::File file) {
        if (window.revealInFinder)
            window.revealInFinder (file);
    };
}

void SongView::perform (Command command)
{
    switch (command)
    {
        case Command::record:
        {
            commitKeyBursts();
            const auto passWasRunning = openSong.isRecording();
            if (auto started = openSong.record(); ! started.has_value())
                window.handleError (started.error());
            else if (! passWasRunning)
                recordPage.clearClipIndicators();
            break;
        }
        case Command::playOrStop: window.handleUnlessRefused (openSong.playOrStop()); break;
        case Command::rewind: openSong.rewind(); break;
        case Command::undo:
            commitKeyBursts();
            if (openSong.package().canUndo())
                window.handleUnlessRefused (openSong.undo());
            break;
        case Command::redo:
            commitKeyBursts();
            if (openSong.package().canRedo())
                window.handleUnlessRefused (openSong.redo());
            break;
        case Command::splitClip:
            if (window.mode() == Mode::edit)
                splitSelectedClip();
            break;
        case Command::deleteSelection:
            if (window.mode() == Mode::edit)
                deleteSelection();
            break;
        case Command::nudgeBack:
            if (window.mode() == Mode::edit)
                nudgePlayhead (-nudgeSeconds);
            break;
        case Command::nudgeForward:
            if (window.mode() == Mode::edit)
                nudgePlayhead (nudgeSeconds);
            break;
        case Command::showRecord:
        case Command::showEdit:
        case Command::showExport:
            break;
    }
}

void SongView::splitSelectedClip()
{
    const auto selected = editPage.timeline.selectedClip();
    if (! selected.has_value())
        return;
    const auto rightHalf = song::makeId();
    if (applyEdit (song::splitClip (openSong.song(), *selected, openSong.position(), rightHalf), song::splitClipLabel))
        editPage.timeline.selectClip (rightHalf);
}

void SongView::deleteSelection()
{
    if (const auto videoTake = editPage.timeline.selectedVideoTake())
        applyEdit (song::deleteVideoTake (openSong.song(), *videoTake), song::deleteVideoTakeLabel);
    else if (const auto clip = editPage.timeline.selectedClip())
        applyEdit (song::deleteClip (openSong.song(), *clip), song::deleteClipLabel);
}

// OpenSong::seek clamps at the Song's start and refuses seeks while a Pass runs.
void SongView::nudgePlayhead (int seconds)
{
    const auto offset = (song::SampleCount) seconds * openSong.song().sampleRate;
    window.handleUnlessRefused (openSong.seek (openSong.position() + offset));
}

bool SongView::applyEdit (core::Expected<song::Song> changed, const std::string& label)
{
    if (! changed.has_value())
    {
        handleOperationError (changed.error());
        return false;
    }
    for (auto* mix : { &editPage.mixer.mix, &recordPage.mix })
    {
        if (! mix->hasKeyBurst())
            continue;
        // `changed` was made from the Song before the burst: commit the burst, then keep its value.
        auto carried = mix->withKeyBurstCarried (std::move (*changed));
        mix->commitKeyBurst();
        changed = std::move (carried);
    }
    auto edited = openSong.edit (std::move (*changed), label);
    window.handleUnlessRefused (edited);
    window.refresh();
    return edited.has_value();
}

void SongView::handleOperationError (const core::Error& error)
{
    // During a Pass, a Separation or a conversion the edit is refused anyway: report the refusal,
    // not the operation's Error.
    window.handleUnlessRefused (openSong.refusalOf (Gesture::edit).value_or (error));
}

void SongView::armCamera (bool armed)
{
    // No camera access prompt for an arm the running activity would refuse.
    if (openSong.refusalOf (Gesture::setCameraArmed).has_value())
        return;
    if (! armed || ! window.askCameraAccess)
        return setCameraArmed (armed);

    window.askCameraAccess ([safeThis = juce::Component::SafePointer<SongView> (this)] (bool granted) {
        if (safeThis == nullptr)
            return;
        if (granted)
            return safeThis->setCameraArmed (true);
        safeThis->window.handleError (core::Error { "capture.camera_access_denied", "the app may not use the camera", {} });
        safeThis->window.refresh();
    });
}

void SongView::setCameraArmed (bool armed)
{
    window.handleUnlessRefused (openSong.setCameraArmed (armed));
    window.refresh();
}

void SongView::setMute (const song::Id& trackId, bool mute)
{
    applyEdit (song::setTrackMute (openSong.song(), trackId, mute), song::muteLabel (openSong.song(), trackId, mute));
}

void SongView::setSolo (const song::Id& trackId, bool solo)
{
    applyEdit (song::setTrackSolo (openSong.song(), trackId, solo), song::soloLabel (openSong.song(), trackId, solo));
}

void SongView::jumpToPass (const song::Id& passId)
{
    const auto& passes = openSong.song().passes;
    const auto pass = std::ranges::find (passes, passId, &song::Pass::id);
    if (! OVERDUB_CHECK (pass != passes.end(), "ui.pass_row_unknown", { "pass_id", passId }))
        return;
    window.handleUnlessRefused (openSong.seek (pass->startSample));
    window.refresh();
}

void SongView::addTrack()
{
    // Ready to record: Armed on the device's first input, or on none when it has no inputs.
    auto inputChannels = openSong.audio().inputChannelNames.empty() ? std::vector<int> {} : std::vector<int> { 0 };
    auto added = song::addTrack (openSong.song(), std::move (inputChannels), song::randomPick());
    const core::Fields fields { { "track_id", added.trackId }, { "input_channels", added.song.tracks.back().inputChannels } };
    if (! applyEdit (std::move (added.song), added.label))
        return;
    core::logEvent (core::Level::info, "ui.track_added", fields);
    takeKeyboardFocus();
}

void SongView::askToImport()
{
    if (! openSong.canChangeTracks() || ! window.chooseFilesToImport)
        return;
    window.chooseFilesToImport ([safeThis = juce::Component::SafePointer<SongView> (this)] (std::vector<juce::File> files) {
        if (safeThis != nullptr)
            safeThis->importFiles (files);
    });
}

void SongView::importFiles (const std::vector<juce::File>& files)
{
    if (files.empty())
        return;
    commitKeyBursts();
    for (const auto& result : openSong.importFiles (files))
        if (! result.has_value())
            window.handleUnlessRefused (result.error());
    window.refresh();
    takeKeyboardFocus();
}

void SongView::askToDeleteTrack (const song::Id& trackId)
{
    if (! openSong.canChangeTracks() || ! window.confirmTrackDelete)
        return;
    const auto* track = song::findTrack (openSong.song(), trackId);
    if (track == nullptr)
        return;
    const auto question = juce::String::fromUTF8 (("Delete Track \"" + track->name + "\"? Its Clips and effects will be removed. You can undo this.").c_str());
    window.confirmTrackDelete (question, [safeThis = juce::Component::SafePointer<SongView> (this), trackId] (bool confirmed) {
        if (safeThis != nullptr && confirmed)
            safeThis->deleteTrack (trackId);
    });
}

void SongView::deleteTrack (const song::Id& trackId)
{
    // The answer comes later: by then the Song may be locked or the Track gone (undo).
    if (! openSong.canChangeTracks())
        return;
    auto deleted = song::deleteTrack (openSong.song(), trackId);
    if (! deleted.has_value())
        return;
    if (! applyEdit (std::move (deleted->song), deleted->label))
        return;
    // Monitoring is transient: a deleted Track stops, and undo brings it back not Monitoring.
    window.handleUnlessRefused (openSong.setMonitoring (trackId, false));
    core::logEvent (core::Level::info, "ui.track_deleted", { { "track_id", trackId }, { "label", deleted->label } });
    takeKeyboardFocus();
}

void SongView::moveTrack (const song::Id& trackId, std::size_t toIndex)
{
    auto moved = song::moveTrack (openSong.song(), trackId, toIndex);
    if (! moved.has_value())
    {
        handleOperationError (moved.error());
        return;
    }
    if (! moved->has_value())
        return; // dropped back in its own slot: no undo step, no save
    if (! applyEdit (std::move ((*moved)->song), (*moved)->label))
        return;
    core::logEvent (core::Level::info, "ui.track_moved", { { "track_id", trackId }, { "to", toIndex } });
}

void SongView::showTrackMenu (const song::Id& trackId)
{
    // The run dialog is modal: no menu opens under it.
    if (runShowing() || ! window.chooseTrackMenuItem)
        return;
    auto items = trackMenuFor (openSong.song(), trackId, TrackMenuState { .passRunning = openSong.isRecording(), .exportRunning = openSong.isExporting() });
    window.chooseTrackMenuItem (items, [safeThis = juce::Component::SafePointer<SongView> (this), trackId, items] (std::optional<std::size_t> picked) {
        if (safeThis == nullptr || ! picked.has_value() || *picked >= items.size() || ! items[*picked].enabled)
            return;
        switch (items[*picked].action)
        {
            case TrackMenuAction::separate: safeThis->startSeparation (trackId); break;
        }
    });
}

void SongView::startSeparation (const song::Id& trackId)
{
    commitKeyBursts();
    if (auto started = openSong.startSeparation (trackId); ! started.has_value())
        window.handleError (started.error());
    window.refresh();
}

void SongView::cancelRun()
{
    openSong.cancelSeparation();
    openSong.cancelSampleRateConversion();
    window.refresh();
    takeKeyboardFocus();
}

void SongView::convertSampleRate (int toRate)
{
    commitKeyBursts();
    core::logEvent (core::Level::info, "ui.sample_rate_conversion_requested", { { "from_rate", openSong.song().sampleRate }, { "to_rate", toRate } });
    if (auto started = openSong.startSampleRateConversion (toRate); ! started.has_value())
        window.handleError (started.error());
    window.refresh();
}

void SongView::showRun (RunDialog& dialog) const
{
    if (openSong.isSeparating())
        dialog.show (separationDialogText (openSong.separationSourceName(), openSong.separationProgress()), openSong.separationProgress().fraction());
    else if (openSong.isConverting())
        dialog.show (conversionDialogText (openSong.conversionTargetRate(), openSong.conversionFraction()), openSong.conversionFraction());
}

void SongView::startExport (const exporting::ExportRequest& request)
{
    if (auto started = openSong.startExport (request); ! started.has_value())
        window.handleError (started.error());
    window.refresh();
}

void SongView::chooseExportFile (const juce::File& current)
{
    if (! window.chooseExportDestination)
        return;
    window.chooseExportDestination (current, [safeThis = juce::Component::SafePointer<SongView> (this)] (std::optional<juce::File> chosen) {
        if (safeThis == nullptr || ! chosen.has_value())
            return;
        safeThis->exportPage.setDestination (*chosen);
    });
}

void SongView::chooseInsert (const song::Id& trackId)
{
    // No plugin menu for an Insert the running activity would refuse.
    if (openSong.refusalOf (Gesture::addInsert).has_value() || ! window.chooseInsertPlugin)
        return;
    auto plugins = window.listInsertPlugins ? window.listInsertPlugins() : std::vector<plugin::AvailableAudioUnit> {};
    window.chooseInsertPlugin (plugins, [safeThis = juce::Component::SafePointer<SongView> (this), trackId, plugins] (std::optional<std::size_t> picked) {
        if (safeThis == nullptr || ! picked.has_value() || *picked >= plugins.size())
            return;
        safeThis->addInsert (trackId, plugins[*picked]);
    });
}

void SongView::addInsert (const song::Id& trackId, const plugin::AvailableAudioUnit& plugin)
{
    auto added = openSong.addInsert (trackId, plugin);
    if (! added.has_value())
        window.handleUnlessRefused (added.error());
    else
        pendingEditorSlot = *added; // its editor opens once it has loaded
    window.refresh();
}

void SongView::openInsertEditor (const song::Id& slotId)
{
    const auto status = openSong.insertStatus (slotId);
    if (status == plugin::InsertStatus::failed)
    {
        window.showNotice ("ui.plugin_missing");
        return;
    }
    if (status == plugin::InsertStatus::crashed)
    {
        window.showNotice (openSong.insertHost().crashedForGood (slotId) ? "ui.plugin_crashed_repeatedly" : "ui.plugin_crashed");
        return;
    }
    // Loading: it opens when ready (syncPluginWindows).
    pendingEditorSlot = slotId;
    syncPluginWindows();
}

void SongView::syncPluginWindows()
{
    const auto editable = ! openSong.isRecording();
    const auto stateOf = [this, editable] (const song::Id& slotId) { return pluginWindowStateFor (openSong.song(), slotId, {}, editable); };
    pluginWindows.sync (stateOf, [this] (const song::Id& slotId) { return openSong.insertHost().processor (slotId); });

    if (! pendingEditorSlot.has_value())
        return;
    const auto slotId = *pendingEditorSlot;
    const auto status = openSong.insertStatus (slotId);
    if (status == plugin::InsertStatus::loading)
        return;
    pendingEditorSlot.reset();
    if (status != plugin::InsertStatus::ready)
        return; // gone from the Song, or failed to load (logged where it failed)
    auto processor = openSong.insertHost().sharedProcessor (slotId);
    if (const auto state = pluginWindowStateFor (openSong.song(), slotId, plugin::hostingBadge (*processor), editable))
        pluginWindows.open (std::move (processor), *state);
}

void SongView::refresh (TopBarState& state)
{
    const auto mode = window.mode();
    const auto& song = openSong.song();
    const auto& package = openSong.package();
    const auto recording = openSong.isRecording();
    state.songOpen = true;
    state.playing = openSong.isPlaying();
    const auto saving = openSong.isSavingPass();
    state.recording = recording && ! saving; // the top bar reads REC only while capturing
    const auto recordPoint = openSong.preRollRecordPoint();
    const auto position = openSong.position();
    state.clock = clockText (position, song.sampleRate);
    state.status = statusText (TransportStatus {
        .playing = state.playing,
        .recording = state.recording,
        .saving = saving,
        .passNumber = openSong.recordingPassNumber(),
        .preRollRemaining = recordPoint.has_value() ? std::optional<song::SampleCount> (*recordPoint - position) : std::nullopt,
        .sampleRate = song.sampleRate,
    });
    state.undoText = undoButtonText (package.canUndo() ? package.undoLabel() : "");
    state.redoText = redoButtonText (package.canRedo() ? package.redoLabel() : "");
    state.canUndo = package.canUndo() && ! recording;
    state.canRedo = package.canRedo() && ! recording;

    recordPage.show (recordPageStateFor (
                         song,
                         openSong.audio().inputChannelNames,
                         [this] (const song::Id& trackId) { return openSong.isMonitoring (trackId); },
                         recording,
                         [this] (const song::Id& slotId) { return openSong.insertStatus (slotId); }),
                     song);
    recordPage.showCamera (cameraControlsStateFor (song, openSong.cameraDevices(), recording), openSong.cameraPreviewLayer());
    recordPage.showStagePlayback (! stageShowsLiveFeed (song.cameraArmed, recording, state.playing));
    if (mode == Mode::record)
        recordPage.showSong (song, RecordTransport { position, openSong.runningPassStart(), recordPoint }, ! recording);
    if (mode == Mode::edit)
        editPage.show (TimelineState { song, openSong.position(), state.playing, ! recording, openSong.canChangeTracks(), openSong.videoPlayback().spans (song) });
    const auto exportPhase = openSong.exportPhase();
    exportPage.show (ExportPageState {
        .clipsEnd = exporting::lastClipEnd (song),
        .sampleRate = song.sampleRate,
        .waitingForAudio = exportPhase == ExportPhase::waitingForAudio,
        .rendering = exportPhase == ExportPhase::rendering,
        .progress = openSong.exportProgress(),
        .finishedFile = openSong.lastExportedFile(),
        .videoTakes = openSong.videoPlayback().spans (song),
        .storedVideoTakeId = song.exportVideoTakeId,
    });
    syncPluginWindows();

    recordPage.setVisible (mode == Mode::record);
    editPage.setVisible (mode == Mode::edit);
    exportPage.setVisible (mode == Mode::exportSong);
}

void SongView::poll (song::SongPackage::Clock::time_point now)
{
    editPage.mixer.mix.commitKeyBurstIfDue (now);
    recordPage.mix.commitKeyBurstIfDue (now);
    handleErrors (openSong.poll (now));
}

void SongView::showMeters()
{
    const auto peaks = openSong.meterPeaks();
    editPage.mixer.showMeters (peaks);
    recordPage.showMeters (peaks);
}

void SongView::refreshVideo (std::uint64_t nowHostNs, std::uint64_t refreshNs, VideoScreen* preview)
{
    const auto stageShows = window.mode() == Mode::record && recordPage.showsStagePlayback();
    if (preview == nullptr && ! stageShows)
        return;
    const auto heard = video::heardSongSample (openSong.playbackClock(), nowHostNs, refreshNs);
    const auto frame = openSong.videoPlayback().frameAt (openSong.song(), heard);
    if (preview != nullptr)
        preview->show (frame.choice, frame.sampleBuffer);
    if (stageShows)
        recordPage.stageVideo.show (frame.choice, frame.sampleBuffer);
}

bool SongView::handleKey (const juce::KeyPress& key, song::SongPackage::Clock::time_point now)
{
    if (key.isKeyCode (juce::KeyPress::escapeKey) && editPage.timeline.cancelGesture ("escape"))
        return true;
    if (key.isKeyCode (juce::KeyPress::escapeKey) && recordPage.cancelTrackDrag())
    {
        core::logEvent (core::Level::debug, "ui.track_drag_cancelled", { { "page", "record" }, { "reason", "escape" } });
        return true;
    }
    const auto mode = window.mode();
    if (mode == Mode::edit && editPage.mixer.mix.handleKey (key, now))
        return true;
    return mode == Mode::record && recordPage.mix.handleKey (key, now);
}

void SongView::releaseFocusOutside (const juce::Component* clicked)
{
    if (! editPage.mixer.isFocusedControl (clicked))
        editPage.mixer.mix.releaseFocus();
    if (! recordPage.isFocusedControl (clicked))
        recordPage.mix.releaseFocus();
}

void SongView::releaseFocus()
{
    editPage.mixer.mix.releaseFocus();
    recordPage.mix.releaseFocus();
}

void SongView::commitKeyBursts()
{
    editPage.mixer.mix.commitKeyBurst();
    recordPage.mix.commitKeyBurst();
}

void SongView::handleErrors (const std::vector<core::Error>& errors)
{
    for (const auto& error : errors)
        window.handleError (error);
}

void SongView::takeKeyboardFocus()
{
    if (window.isShowing())
        window.grabKeyboardFocus();
}

void SongView::resized()
{
    for (auto* page : std::initializer_list<juce::Component*> { &recordPage, &editPage, &exportPage })
        page->setBounds (getLocalBounds());
}
} // namespace overdub::ui
