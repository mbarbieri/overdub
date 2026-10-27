#include "ui/SongView.h"

#include "core/Log.h"
#include "export/MixRender.h"
#include "plugin/PluginKind.h"
#include "song/ClipEdits.h"
#include "song/InsertEdits.h"
#include "song/MixEdits.h"
#include "song/TakeEdits.h"
#include "song/TrackEdits.h"
#include "song/Transpose.h"
#include "song/VideoTakeEdits.h"

#include "ui/MainComponent.h"
#include "ui/TakeLanes.h"
#include "ui/TrackRowEdits.h"
#include "ui/TransportText.h"
#include "ui/VideoScreen.h"

#include <algorithm>
#include <memory>

namespace overdub::ui
{
namespace
{
core::Error quiet (std::string code, std::string message, core::Fields fields = {})
{
    return core::Error { std::move (code), std::move (message), std::move (fields) };
}
} // namespace

SongView::SongView (OpenSong& song, MainComponent& mainWindow)
    : openSong (song),
      window (mainWindow),
      recordPage ([this] (const song::ClipSource& source) -> const song::Peaks& { return openSong.peaks (source); }),
      editPage ([this] (const song::ClipSource& source) -> const song::Peaks& { return openSong.peaks (source); }),
      pluginWindows (mainWindow.showPluginWindowsOnDesktop)
{
    wirePages();

    addChildComponent (recordPage);
    addChildComponent (editPage);
}

SongView::~SongView() = default;

void SongView::wirePages()
{
    recordPage.onIntent = [this] (const Intent& intent) { window.run (intent); };
    recordPage.onTrackMenu = [this] (const song::Id& trackId) { showTrackMenu (trackId); };
    recordPage.onImport = [this] { askToImport(); };

    recordPage.onAddInsert = [this] (const song::Id& trackId) { chooseInsert (trackId); };
    editPage.onAddInsert = [this] (const song::Id& trackId) { chooseInsert (trackId); };
    editPage.mixer.insertStatusOf = [this] (const song::Id& slotId) { return openSong.insertStatus (slotId); };
    pluginWindows.onIntent = [this] (const Intent& intent) { window.run (intent); };

    editPage.onIntent = [this] (const Intent& intent) { window.run (intent); };
    editPage.onDeleteTrack = [this] (const song::Id& trackId) { askToDeleteTrack (trackId); };
    editPage.timeline.canImportFiles = [this] { return ! window.refusalOf (intent::ImportFiles::op).has_value(); };

    exportSheet.onIntent = [this] (const Intent& intent) { window.run (intent); };
    exportSheet.onChooseDestination = [this] (juce::File current) { chooseExportFile (current); };
}

IntentResult SongView::run (const SongIntent& songIntent)
{
    using namespace intent;
    return core::visitExhaustively (
        songIntent,
        core::Overloaded {
            [this] (const Edit& plainEdit) { return edit (plainEdit); },
            [this] (const Record&) { return record(); },
            [this] (const RecordAgain&) { return recordAgain(); },
            [this] (const PlayOrStop&) { return unlessRefused (openSong.playOrStop()); },
            [this] (const Rewind&) -> IntentResult { openSong.rewind(); return IntentDone {}; },
            [this] (const Undo&) { return undo(); },
            [this] (const Redo&) { return redo(); },
            [this] (const Seek& i) { return seek (i.sample); },
            [this] (const JumpToPass& i) { return jumpToPass (i.passId); },
            [this] (const Nudge& i) { return nudgePlayhead (i.direction); },
            [this] (const AddTrack&) { return addTrack(); },
            [this] (const ImportFiles& i) { return importFiles (i.files); },
            [this] (const DeleteTrack& i) { return deleteTrack (i.trackId); },
            [this] (const MoveTrack& i) { return moveTrack (i.trackId, i.toIndex); },
            [this] (const SetArmed& i) { return setArmed (i.trackId, i.armed); },
            [this] (const SetInputChannels& i) { return setInputChannels (i.trackId, i.channels); },
            [this] (const SetMonitoring& i) { return setMonitoring (i.trackId, i.on); },
            [this] (const StartSeparation& i) { return startSeparation (i.trackId); },
            [this] (const CancelRun&) { return cancelRun(); },
            [this] (const SetTranspose& i) { return setTranspose (i.semitones); },
            [this] (const SetPreRoll& i) { return setPreRoll (i.preRoll); },
            [this] (const SetCameraArmed& i) { return armCamera (i.armed); },
            [this] (const SetCameraDevice& i) { return setCameraDevice (i.cameraId); },
            [this] (const SplitSelectedClip&) { return splitSelectedClip(); },
            [this] (const DeleteSelection&) { return deleteSelection(); },
            [this] (const TakeLaneClick& i) { return takeLaneClick (i.takeId, i.at); },
            [this] (const PreviewMix& i) { return previewMix (i); },
            [this] (const AddInsert& i) { return addInsert (i); },
            [this] (const OpenInsertEditor& i) { return openInsertEditor (i.slotId); },
            [this] (const ClosePluginWindow& i) { return closePluginWindow (i.slotId); },
            [this] (const ShowExport& i) { return showExport (i.shown); },
            [this] (const SetExportFormat& i) { return setExportFormat (i.format); },
            [this] (const SetExportLength& i) { return setExportLength (i.length); },
            [this] (const SetExportDestination& i) { return setExportDestination (juce::File (i.path.value)); },
            [this] (const SetExportVideoTake& i) { return setExportVideoTake (i.videoTakeId); },
            [this] (const Export&) { return exportSong(); },
            [this] (const CancelExport&) { return cancelExport(); },
            [this] (const RevealExport&) { return revealExport(); },
            [this] (const SelectClip& i) -> IntentResult { editPage.timeline.selectClip (i.clipId); return IntentDone {}; },
            [this] (const SelectVideoTake& i) -> IntentResult { editPage.timeline.selectVideoTake (i.videoTakeId); return IntentDone {}; },
            [this] (const SetTimelineView& i) -> IntentResult { editPage.timeline.setView (i.firstSample, i.samplesPerPixel); return IntentDone {}; },
            [this] (const SetVerticalScroll& i) -> IntentResult { editPage.timeline.setVerticalScroll (i.pixels); return IntentDone {}; },
            [this] (const SetTakeLanesUnfolded& i) -> IntentResult { editPage.timeline.setTakeLanesUnfolded (i.trackId, i.unfolded); return IntentDone {}; },
            [this] (const SetMixerVisible& i) -> IntentResult { editPage.setMixerVisible (i.visible); return IntentDone {}; },
            [this] (const SetMixerHeight& i) -> IntentResult { editPage.setMixerHeight (i.height); return IntentDone {}; },
            [this] (const SetPassesPanelOpen& i) -> IntentResult { editPage.setPassesPanelOpen (i.open); return IntentDone {}; },
            [this] (const ClearClipIndicator& i) { return clearClipIndicator (i.trackId); },
        });
}

IntentResult SongView::unlessRefused (const core::Expected<void>& result)
{
    window.handleUnlessRefused (result);
    if (! result.has_value())
        return std::unexpected (result.error());
    return IntentDone {};
}

IntentResult SongView::presented (const core::Expected<void>& result)
{
    if (! result.has_value())
    {
        window.handleError (result.error());
        return std::unexpected (result.error());
    }
    return IntentDone {};
}

IntentResult SongView::record()
{
    const auto passWasRunning = openSong.isRecording();
    auto started = presented (openSong.record());
    if (started.has_value() && ! passWasRunning)
        recordPage.clearClipIndicators();
    return started;
}

IntentResult SongView::recordAgain()
{
    const auto passWasRunning = openSong.isRecording();
    auto started = presented (openSong.recordAgain());
    if (started.has_value() && ! passWasRunning)
        recordPage.clearClipIndicators();
    return started;
}

IntentResult SongView::undo()
{
    if (! openSong.package().canUndo())
        return window.quietError (quiet ("ui.nothing_to_undo", "there is no step to undo"));
    return unlessRefused (openSong.undo());
}

IntentResult SongView::redo()
{
    if (! openSong.package().canRedo())
        return window.quietError (quiet ("ui.nothing_to_redo", "there is no step to redo"));
    return unlessRefused (openSong.redo());
}

IntentResult SongView::seek (song::SampleCount sample)
{
    auto sought = unlessRefused (openSong.seek (sample));
    window.refresh();
    return sought;
}

IntentResult SongView::splitSelectedClip()
{
    if (window.mode() != Mode::edit)
        return window.quietError (quiet ("ui.not_in_edit_mode", "the Clip keys act in Edit mode only", { { "op", std::string (intent::SplitSelectedClip::op) } }));
    const auto selected = editPage.timeline.selectedClip();
    if (! selected.has_value())
        return window.quietError (quiet ("ui.no_clip_selected", "no Clip is selected"));
    const auto rightHalf = song::makeId();
    if (auto split = applyEdit (song::splitClip (openSong.song(), *selected, openSong.position(), rightHalf), song::splitClipLabel); ! split.has_value())
        return split;
    editPage.timeline.selectClip (rightHalf);
    return IntentDone { .createdId = rightHalf };
}

IntentResult SongView::deleteSelection()
{
    if (window.mode() != Mode::edit)
        return window.quietError (quiet ("ui.not_in_edit_mode", "the Clip keys act in Edit mode only", { { "op", std::string (intent::DeleteSelection::op) } }));
    if (const auto videoTake = editPage.timeline.selectedVideoTake())
        return applyEdit (song::deleteVideoTake (openSong.song(), *videoTake), song::deleteVideoTakeLabel);
    if (const auto clip = editPage.timeline.selectedClip())
        return applyEdit (song::deleteClip (openSong.song(), *clip), song::deleteClipLabel);
    return window.quietError (quiet ("ui.nothing_selected", "no Clip or Video Take is selected"));
}

// OpenSong::seek clamps at the Song's start and refuses seeks while a Pass runs.
IntentResult SongView::nudgePlayhead (Direction direction)
{
    if (window.mode() != Mode::edit)
        return window.quietError (quiet ("ui.not_in_edit_mode", "the arrow keys nudge in Edit mode only", { { "op", std::string (intent::Nudge::op) } }));
    const auto seconds = direction == Direction::back ? -nudgeSeconds : nudgeSeconds;
    const auto offset = (song::SampleCount) seconds * openSong.song().sampleRate;
    return unlessRefused (openSong.seek (openSong.position() + offset));
}

IntentResult SongView::applyEdit (core::Expected<song::Song> changed, const std::string& label)
{
    if (! changed.has_value())
        return std::unexpected (handleOperationError (changed.error()));
    auto edited = unlessRefused (openSong.edit (std::move (*changed), label));
    window.refresh();
    return edited;
}

IntentResult SongView::edit (const intent::Edit& plainEdit)
{
    auto edited = editedSong (openSong.song(), plainEdit);
    if (! edited.has_value())
        return std::unexpected (handleOperationError (edited.error()));
    return applyEdit (std::move (edited->song), edited->label);
}

core::Error SongView::handleOperationError (const core::Error& error)
{
    window.handleError (error);
    return error;
}

IntentResult SongView::setArmed (const song::Id& trackId, bool armed)
{
    auto result = unlessRefused (openSong.setArmed (trackId, armed));
    window.refresh();
    return result;
}

IntentResult SongView::setInputChannels (const song::Id& trackId, std::vector<int> channels)
{
    // The input picker offers only inputChoices, so only the socket can name anything else.
    const auto& inputNames = openSong.audio().inputChannelNames;
    if (! std::ranges::contains (inputChoices (inputNames), channels, &InputChoice::channels))
        return window.quietError (quiet ("ui.input_channels_unavailable",
                                         "the audio device offers no such input",
                                         { { "track_id", trackId }, { "channels", channels }, { "input_count", inputNames.size() } }));
    auto result = unlessRefused (openSong.setInputChannels (trackId, std::move (channels)));
    window.refresh();
    takeKeyboardFocus();
    return result;
}

IntentResult SongView::setMonitoring (const song::Id& trackId, bool on)
{
    auto result = unlessRefused (openSong.setMonitoring (trackId, on));
    window.refresh();
    return result;
}

IntentResult SongView::setPreRoll (song::PreRoll preRoll)
{
    auto result = unlessRefused (openSong.setPreRoll (preRoll));
    window.refresh();
    return result;
}

IntentResult SongView::setTranspose (int semitones)
{
    if (semitones == openSong.song().transpose)
        return IntentDone {};
    return applyEdit (song::setTranspose (openSong.song(), semitones), song::transposeLabel (semitones));
}

IntentResult SongView::setCameraDevice (const std::string& cameraId)
{
    auto result = unlessRefused (openSong.setCameraDevice (cameraId));
    window.refresh();
    return result;
}

IntentResult SongView::armCamera (bool armed)
{
    if (! armed || ! window.askCameraAccess)
        return setCameraArmed (armed);

    // The answer may come at once (tests) or after the system's prompt.
    auto answer = std::make_shared<std::optional<IntentResult>>();
    window.askCameraAccess ([safeThis = juce::Component::SafePointer<SongView> (this), answer] (bool granted) {
        if (safeThis != nullptr)
            *answer = safeThis->cameraAccessAnswered (granted);
    });
    if (answer->has_value())
        return **answer;
    return IntentDone { .awaitingAnswer = true };
}

IntentResult SongView::cameraAccessAnswered (bool granted)
{
    if (granted)
        return setCameraArmed (true);
    auto denied = presented (std::unexpected (core::Error { "capture.camera_access_denied", "the app may not use the camera", {} }));
    window.refresh();
    return denied;
}

IntentResult SongView::setCameraArmed (bool armed)
{
    auto result = unlessRefused (openSong.setCameraArmed (armed));
    window.refresh();
    return result;
}

IntentResult SongView::jumpToPass (const song::Id& passId)
{
    const auto& passes = openSong.song().passes;
    const auto pass = std::ranges::find (passes, passId, &song::Pass::id);
    if (pass == passes.end())
        return window.quietError (quiet ("ui.pass_not_found", "the Song has no such Pass", { { "pass_id", passId } }));
    return seek (pass->startSample);
}

IntentResult SongView::addTrack()
{
    // Ready to record: Armed on the device's first input, or on none when it has no inputs.
    auto inputChannels = openSong.audio().inputChannelNames.empty() ? std::vector<int> {} : std::vector<int> { 0 };
    auto added = song::addTrack (openSong.song(), std::move (inputChannels), song::randomPick());
    const core::Fields fields { { "track_id", added.trackId }, { "input_channels", added.song.tracks.back().inputChannels } };
    if (auto applied = applyEdit (std::move (added.song), added.label); ! applied.has_value())
        return applied;
    core::logEvent (core::Level::info, "ui.track_added", fields);
    takeKeyboardFocus();
    return IntentDone { .createdId = added.trackId };
}

void SongView::askToImport()
{
    if (! window.chooseFilesToImport || ! window.mayRun (intent::ImportFiles::op))
        return;
    window.chooseFilesToImport ([safeThis = juce::Component::SafePointer<SongView> (this)] (std::vector<juce::File> files) {
        if (safeThis == nullptr || files.empty())
            return;
        intent::ImportFiles import;
        for (const auto& file : files)
            import.files.push_back (core::AbsolutePath { file.getFullPathName().toStdString() });
        safeThis->window.run (SongIntent { std::move (import) });
    });
}

IntentResult SongView::importFiles (const std::vector<core::AbsolutePath>& paths)
{
    if (paths.empty())
        return window.quietError (quiet ("ui.no_files_to_import", "no files to import"));
    std::vector<juce::File> files;
    for (const auto& path : paths)
        files.push_back (juce::File (path.value));
    IntentResult imported = IntentDone {};
    for (const auto& result : openSong.importFiles (files))
        if (! result.has_value())
        {
            window.handleUnlessRefused (result.error());
            if (imported.has_value())
                imported = std::unexpected (result.error());
        }
    window.refresh();
    takeKeyboardFocus();
    return imported;
}

void SongView::askToDeleteTrack (const song::Id& trackId)
{
    if (! window.confirmTrackDelete || ! window.mayRun (intent::DeleteTrack::op))
        return;
    const auto* track = song::findTrack (openSong.song(), trackId);
    if (track == nullptr)
        return;
    const auto question = juce::String::fromUTF8 (("Delete Track \"" + track->name + "\"? Its Clips and effects will be removed. You can undo this.").c_str());
    window.confirmTrackDelete (question, [safeThis = juce::Component::SafePointer<SongView> (this), trackId] (bool confirmed) {
        if (safeThis != nullptr && confirmed)
            safeThis->window.run (SongIntent { intent::DeleteTrack { trackId } });
    });
}

IntentResult SongView::deleteTrack (const song::Id& trackId)
{
    // The answer comes later: by then the Track may be gone (undo).
    auto deleted = song::deleteTrack (openSong.song(), trackId);
    if (! deleted.has_value())
        return window.quietError (deleted.error());
    if (auto applied = applyEdit (std::move (deleted->song), deleted->label); ! applied.has_value())
        return applied;
    // Monitoring is transient: a deleted Track stops, and undo brings it back not Monitoring.
    window.handleUnlessRefused (openSong.setMonitoring (trackId, false));
    core::logEvent (core::Level::info, "ui.track_deleted", { { "track_id", trackId }, { "label", deleted->label } });
    takeKeyboardFocus();
    return IntentDone {};
}

IntentResult SongView::moveTrack (const song::Id& trackId, std::size_t toIndex)
{
    auto moved = song::moveTrack (openSong.song(), trackId, toIndex);
    if (! moved.has_value())
        return std::unexpected (handleOperationError (moved.error()));
    if (! moved->has_value())
        return window.quietError (quiet ("ui.track_already_there", "the Track is already at that place", { { "track_id", trackId }, { "to_index", toIndex } }));
    if (auto applied = applyEdit (std::move ((*moved)->song), (*moved)->label); ! applied.has_value())
        return applied;
    core::logEvent (core::Level::info, "ui.track_moved", { { "track_id", trackId }, { "to", toIndex } });
    return IntentDone {};
}

void SongView::showTrackMenu (const song::Id& trackId)
{
    // The run dialog is modal: no menu opens under it.
    if (runShowing() || ! window.chooseTrackMenuItem)
        return;
    const auto refused = window.refusalOf (intent::StartSeparation::op);
    const auto separationRefusal = refused.has_value() ? std::optional<std::string> (refused->error.code) : std::nullopt;
    auto items = trackMenuFor (openSong.song(), trackId, TrackMenuState { .separationRefusal = separationRefusal });
    window.chooseTrackMenuItem (items, [safeThis = juce::Component::SafePointer<SongView> (this), trackId, items] (std::optional<std::size_t> picked) {
        if (safeThis == nullptr || ! picked.has_value() || *picked >= items.size() || ! items[*picked].enabled)
            return;
        switch (items[*picked].action)
        {
            case TrackMenuAction::separate: safeThis->window.run (SongIntent { intent::StartSeparation { trackId } }); break;
        }
    });
}

IntentResult SongView::startSeparation (const song::Id& trackId)
{
    auto started = presented (openSong.startSeparation (trackId));
    window.refresh();
    return started;
}

IntentResult SongView::cancelRun()
{
    openSong.cancelSeparation();
    openSong.cancelSampleRateConversion();
    window.refresh();
    takeKeyboardFocus();
    return IntentDone {};
}

IntentResult SongView::convertSampleRate (int toRate)
{
    core::logEvent (core::Level::info, "ui.sample_rate_conversion_requested", { { "from_rate", openSong.song().sampleRate }, { "to_rate", toRate } });
    auto started = presented (openSong.startSampleRateConversion (toRate));
    window.refresh();
    return started;
}

void SongView::showRun (RunDialog& dialog) const
{
    if (openSong.isSeparating())
        dialog.show (separationDialogText (openSong.separationSourceName(), openSong.separationProgress()), openSong.separationProgress().fraction());
    else if (openSong.isConverting())
        dialog.show (conversionDialogText (openSong.conversionTargetRate(), openSong.conversionFraction()), openSong.conversionFraction());
}

IntentResult SongView::takeLaneClick (const song::Id& takeId, song::SampleCount at)
{
    auto laneEdit = laneEditAt (openSong.song(), takeId, at, song::makeId());
    if (! laneEdit.has_value())
        return window.quietError (quiet ("ui.take_lane_click_changes_nothing", "the Take is already heard there", { { "take_id", takeId }, { "at", at } }));
    if (auto applied = applyEdit (std::move (laneEdit->song), laneEdit->label); ! applied.has_value())
        return applied;
    editPage.timeline.selectClip (laneEdit->clipId);
    return IntentDone { .createdId = laneEdit->clipId };
}

IntentResult SongView::previewMix (const intent::PreviewMix& preview)
{
    if (! preview.edit.has_value())
        return unlessRefused (openSong.previewMix (openSong.song()));
    auto edited = editedSong (openSong.song(), asEdit (*preview.edit));
    if (! edited.has_value())
        return window.quietError (edited.error());
    return unlessRefused (openSong.previewMix (edited->song));
}

IntentResult SongView::showExport (bool shown)
{
    setExportSheetShown (shown);
    window.refresh();
    return IntentDone {};
}

void SongView::setExportSheetShown (bool shown)
{
    if (shown != exportSheetShown)
        core::logEvent (core::Level::debug, "ui.export_sheet_shown", { { "shown", shown } });
    exportSheetShown = shown;
    if (! shown)
        takeKeyboardFocus();
}

exporting::ExportRequest SongView::exportRequest() const
{
    const auto& song = openSong.song();
    // The sheet falls back to the export folder; Export without it (the socket) keeps the stored
    // destination, and fails with export.folder_missing when that folder is gone.
    const auto destination = exportDestinationFor (song, window.exportFolder, exportSongName(), exportSheetShown);
    return exportRequestFor (song, openSong.videoPlayback().spans (song), destination);
}

std::string SongView::exportSongName() const
{
    return openSong.package().location().getFileNameWithoutExtension().toStdString();
}

IntentResult SongView::exportSong()
{
    const auto request = exportRequest();
    // Only the default destination's folder (the export folder) is made when missing; any other is
    // the user's, and a gone one is startExport's export.folder_missing.
    if (request.destination == exporting::defaultExportFile (window.exportFolder, exportSongName(), request.format))
        if (auto made = exporting::createExportFolder (request.destination); ! made.has_value())
            return presented (made);
    auto started = presented (openSong.startExport (request));
    // The top bar shows the export's progress from here on.
    if (started.has_value())
        setExportSheetShown (false);
    window.refresh();
    return started;
}

IntentResult SongView::setExportFormat (exporting::ExportFormat format)
{
    openSong.setExportFormat (format);
    window.refresh();
    return IntentDone {};
}

IntentResult SongView::setExportLength (exporting::VideoLength length)
{
    openSong.setExportLength (length);
    window.refresh();
    return IntentDone {};
}

IntentResult SongView::setExportDestination (const juce::File& file)
{
    if (auto folder = exporting::checkExportFolder (file); ! folder.has_value())
        return presented (folder);
    openSong.setExportDestination (file.getFullPathName().toStdString());
    window.refresh();
    return IntentDone {};
}

IntentResult SongView::setExportVideoTake (const song::Id& videoTakeId)
{
    openSong.setExportVideoTake (videoTakeId);
    window.refresh();
    return IntentDone {};
}

IntentResult SongView::cancelExport()
{
    if (! openSong.isExporting())
        return window.quietError (quiet ("ui.no_export_running", "no export is running"));
    const auto ended = openSong.cancelExport();
    window.refresh();
    if (! ended.has_value())
    {
        showExportFinished();
        return IntentDone {};
    }
    if (ended->code != "ui.export_cancelled")
        return presented (std::unexpected (*ended));
    window.showNotice (ended->code);
    return IntentDone {};
}

void SongView::showExportFinished()
{
    const auto& file = openSong.lastExportedFile();
    if (! file.has_value())
        return;
    window.showNotice (exportFinishedCode, { { "file_name", file->getFileName().toStdString() }, { "path", file->getFullPathName().toStdString() } });
}

IntentResult SongView::revealExport()
{
    const auto& file = openSong.lastExportedFile();
    if (! file.has_value())
        return window.quietError (quiet ("ui.nothing_exported", "no export has written a file yet"));
    if (window.revealInFinder)
        window.revealInFinder (*file);
    return IntentDone {};
}

void SongView::chooseExportFile (const juce::File& current)
{
    if (! window.chooseExportDestination)
        return;
    window.chooseExportDestination (current, [safeThis = juce::Component::SafePointer<SongView> (this)] (std::optional<juce::File> chosen) {
        if (safeThis == nullptr || ! chosen.has_value())
            return;
        safeThis->window.run (SongIntent { intent::SetExportDestination { core::AbsolutePath { chosen->getFullPathName().toStdString() } } });
    });
}

void SongView::chooseInsert (const song::Id& trackId)
{
    if (! window.chooseInsertPlugin || ! window.mayRun (intent::AddInsert::op))
        return;
    auto plugins = window.listInsertPlugins ? window.listInsertPlugins() : std::vector<plugin::AvailableAudioUnit> {};
    window.chooseInsertPlugin (plugins, [safeThis = juce::Component::SafePointer<SongView> (this), trackId, plugins] (std::optional<std::size_t> picked) {
        if (safeThis == nullptr || ! picked.has_value() || *picked >= plugins.size())
            return;
        const auto& component = plugins[*picked].component;
        safeThis->window.run (SongIntent { intent::AddInsert { trackId, component.type, component.subtype, component.manufacturer } });
    });
}

IntentResult SongView::addInsert (const intent::AddInsert& insert)
{
    const song::AudioUnitComponent component { insert.type, insert.subtype, insert.manufacturer };
    const auto plugins = window.listInsertPlugins ? window.listInsertPlugins() : std::vector<plugin::AvailableAudioUnit> {};
    const auto plugin = std::ranges::find (plugins, component, &plugin::AvailableAudioUnit::component);
    if (plugin == plugins.end())
        return window.quietError (quiet ("ui.plugin_not_installed", "no installed AU effect has this component", { { "type", insert.type }, { "subtype", insert.subtype }, { "manufacturer", insert.manufacturer } }));
    return addInsert (insert.trackId, *plugin);
}

IntentResult SongView::addInsert (const song::Id& trackId, const plugin::AvailableAudioUnit& plugin)
{
    auto added = openSong.addInsert (trackId, plugin);
    if (! added.has_value())
        window.handleUnlessRefused (added.error());
    else
        pendingEditorSlot = *added; // its editor opens once it has loaded
    window.refresh();
    if (! added.has_value())
        return std::unexpected (added.error());
    return IntentDone { .createdId = *added };
}

IntentResult SongView::openInsertEditor (const song::Id& slotId)
{
    const auto status = openSong.insertStatus (slotId);
    if (status == plugin::InsertStatus::failed || status == plugin::InsertStatus::crashed)
    {
        const auto code = status == plugin::InsertStatus::failed        ? "ui.plugin_missing"
                        : openSong.insertHost().crashedForGood (slotId) ? "ui.plugin_crashed_repeatedly"
                                                                        : "ui.plugin_crashed";
        window.showNotice (code);
        return std::unexpected (core::Error { code, "the plugin cannot open its editor", { { "slot_id", slotId } } });
    }
    // Loading: it opens when ready (syncPluginWindows).
    pendingEditorSlot = slotId;
    syncPluginWindows();
    return IntentDone {};
}

IntentResult SongView::closePluginWindow (const song::Id& slotId)
{
    pluginWindows.close (slotId);
    openSong.notePluginStateChanged();
    return IntentDone {};
}

IntentResult SongView::clearClipIndicator (const song::Id& trackId)
{
    const auto row = std::ranges::find (recordPage.rows, trackId, [] (const auto& candidate) { return candidate->trackId(); });
    if (row == recordPage.rows.end())
        return window.quietError (quiet ("ui.track_not_found", "the Song has no such Track", { { "track_id", trackId } }));
    (*row)->clip.clear();
    return IntentDone {};
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
    state.canUndo = package.canUndo() && ! window.refusalOf (intent::Undo::op).has_value();
    state.canRedo = package.canRedo() && ! window.refusalOf (intent::Redo::op).has_value();
    state.canExport = ! window.refusalOf (intent::ShowExport::op).has_value();
    state.canCancelExport = ! window.refusalOf (intent::CancelExport::op).has_value();
    state.transpose = song.transpose;
    state.transposeRendering = openSong.isTransposeRendering();
    state.canTranspose = ! window.refusalOf (intent::SetTranspose::op).has_value();
    if (openSong.isExporting())
    {
        state.exportStatus = exportStatusText (openSong.exportPhase() == ExportPhase::waitingForAudio, openSong.exportProgress());
        state.exportProgress = openSong.exportProgress();
    }

    recordPage.show (recordPageStateFor (
                         song,
                         openSong.audio().inputChannelNames,
                         [this] (const song::Id& trackId) { return openSong.isMonitoring (trackId); },
                         recording,
                         [this] (const song::Id& slotId) { return openSong.insertStatus (slotId); }),
                     song);
    const auto cameraControls = cameraControlsStateFor (song, openSong.cameraDevices(), recording);
    const auto playback = ! stageShowsLiveFeed (song.cameraArmed, recording, state.playing);
    const auto cameraMessage = juce::String::fromUTF8 (cameraStageMessage (song.cameraArmed).c_str());
    // The camera's preview layer can sit in one view at a time: the page out of view lets go of it
    // before the page in view takes it.
    auto* const previewLayer = openSong.cameraPreviewLayer();
    if (mode == Mode::edit)
    {
        recordPage.showCamera (cameraControls, nullptr);
        editPage.showVideoPreview (song.cameraArmed, previewLayer, cameraMessage, playback);
    }
    else
    {
        editPage.showVideoPreview (song.cameraArmed, nullptr, cameraMessage, playback);
        recordPage.showCamera (cameraControls, mode == Mode::record ? previewLayer : nullptr);
    }
    recordPage.showStagePlayback (playback);
    if (mode == Mode::record)
        recordPage.showSong (song, RecordTransport { position, openSong.runningPassStart(), recordPoint }, ! recording);
    if (mode == Mode::edit)
        editPage.show (TimelineState {
            .song = song,
            .playhead = openSong.position(),
            .playing = state.playing,
            .editable = ! recording,
            .tracksDeletable = ! window.refusalOf (intent::ImportFiles::op).has_value(),
            .videoTakes = openSong.videoPlayback().spans (song),
            .canArmCamera = cameraControls.canArm && ! window.refusalOf (intent::SetCameraArmed::op).has_value(),
        });
    const auto request = exportRequest();
    exportSheet.show (ExportSheetState {
        .clipsEnd = exporting::lastClipEnd (song),
        .sampleRate = song.sampleRate,
        .videoTakes = openSong.videoPlayback().spans (song),
        .request = request,
        .replacesExisting = exportSheetShown && request.destination.existsAsFile(),
    });
    syncPluginWindows();

    recordPage.setVisible (mode == Mode::record);
    editPage.setVisible (mode == Mode::edit);
    exportSheet.setVisible (exportSheetShown);
}

void SongView::poll (song::SongPackage::Clock::time_point now)
{
    editPage.mixer.mix.commitKeyBurstIfDue (now);
    recordPage.mix.commitKeyBurstIfDue (now);
    const auto wasExporting = openSong.isExporting();
    handleErrors (openSong.poll (now));
    if (playAfterSpace.has_value() && now > *playAfterSpace)
    {
        playAfterSpace.reset();
        if (! openSong.isRecording() && ! openSong.isPlaying())
            window.run (SongIntent { intent::PlayOrStop {} });
    }
    if (wasExporting && ! openSong.isExporting())
        showExportFinished();
}

void SongView::showMeters()
{
    shownPeaks = openSong.meterPeaks();
    editPage.mixer.showMeters (shownPeaks);
    recordPage.showMeters (shownPeaks);
}

void SongView::refreshVideo (std::uint64_t nowHostNs, std::uint64_t refreshNs)
{
    const auto mode = window.mode();
    const auto stageShows = mode == Mode::record && recordPage.showsStagePlayback();
    auto* const preview = mode == Mode::edit ? editPage.shownPlayback() : nullptr;
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
    // The Export sheet is modal: Esc closes it, Return exports as its Export button does, and every
    // other key does nothing.
    if (exportSheetShown)
    {
        if (key.isKeyCode (juce::KeyPress::escapeKey))
            window.run (SongIntent { intent::ShowExport { false } });
        else if (key.isKeyCode (juce::KeyPress::returnKey) && exportSheet.canExport())
            window.run (SongIntent { intent::Export {} });
        return true;
    }
    if (key.isKeyCode (juce::KeyPress::escapeKey) && editPage.timeline.cancelGesture ("escape"))
        return true;
    if (key.isKeyCode (juce::KeyPress::escapeKey) && recordPage.cancelTrackDrag())
    {
        core::logEvent (core::Level::debug, "ui.track_drag_cancelled", { { "page", "record" }, { "reason", "escape" } });
        return true;
    }
    const auto mode = window.mode();
    if (mode == Mode::record && key.isKeyCode (juce::KeyPress::spaceKey) && ! key.getModifiers().isAnyModifierKeyDown())
    {
        spaceInRecordMode (now);
        return true;
    }
    if (mode == Mode::edit && editPage.mixer.mix.handleKey (key, now))
        return true;
    return mode == Mode::record && recordPage.mix.handleKey (key, now);
}

void SongView::spaceInRecordMode (song::SongPackage::Clock::time_point now)
{
    const auto interval = window.doubleTapInterval();
    // Another Intent between the two (R, a click) makes this Space a first one again.
    if (lastSpace.has_value() && now - *lastSpace <= interval && window.intentsRun() == intentsRunAtLastSpace)
    {
        lastSpace.reset();
        playAfterSpace.reset();
        window.run (SongIntent { intent::RecordAgain {} });
        return;
    }
    lastSpace = now;
    if (openSong.isRecording() || openSong.isPlaying())
        window.run (SongIntent { intent::PlayOrStop {} });
    else
        playAfterSpace = now + interval;
    intentsRunAtLastSpace = window.intentsRun();
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
    commitKeyBurst (editPage.mixer.mix);
    commitKeyBurst (recordPage.mix);
}

// Not a run of its own: only the Intent that committed the burst is logged, with its origin.
void SongView::commitKeyBurst (MixFocus& mix)
{
    if (auto burst = mix.takeKeyBurst())
        edit (*burst);
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
    for (auto* page : std::initializer_list<juce::Component*> { &recordPage, &editPage })
        page->setBounds (getLocalBounds());
}
} // namespace overdub::ui
