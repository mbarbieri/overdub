#pragma once

#include "core/Error.h"
#include "ui/EditPage.h"
#include "ui/ExportSheet.h"
#include "ui/InsertChain.h"
#include "ui/Intent.h"
#include "ui/OpenSong.h"
#include "ui/PeaksLookup.h"
#include "ui/PluginWindows.h"
#include "ui/RecordPage.h"
#include "ui/RunDialog.h"
#include "ui/SongEdits.h"
#include "ui/TopBar.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace overdub::ui
{
class MainComponent;
class VideoScreen;

// The part of the main window that needs a Song: the Record and Edit pages, the Export sheet and
// the plugin editor windows, wired to one OpenSong. The Export sheet covers the whole window, so
// the window puts it among its own children (MainComponent::setOpenSong) while this object owns
// it; show_export shows and hides it, and an export that starts closes it (issue #200). The
// window (ui/MainComponent.h) builds one when a Song opens and destroys it when the Song closes,
// so nothing in here sees a null Song. A callback
// that answers later (a chooser, a confirmation, camera access) holds a SafePointer to this
// object and is dropped once the Song has closed.
//
// run() is the Song group's one handler (ADR 0008): an exhaustive visit into the private methods
// below, which keep every side effect a gesture has (the right half selected after a split,
// Monitoring off after a Track delete, clip indicators out when a Pass starts, refusals presented
// only for record, export, Separation and conversion). A gesture that does nothing returns an
// Error logged at debug and never presented. Everything reaches it through MainComponent::run: keys, the top bar, the Edit menu, and every
// page (ui/RecordPage.h, ui/EditPage.h, ui/ExportSheet.h) and the plugin windows, which report their
// gestures as Intents through one sink each; their few hooks open a dialog (a chooser, a
// confirmation, a menu) whose answer runs an Intent the same way.
//
// `song` and `window` must outlive this object. Message thread only.
class SongView final : public juce::Component
{
    OpenSong& openSong;
    MainComponent& window;

public:
    SongView (OpenSong& song, MainComponent& window);
    ~SongView() override;

    OpenSong& currentSong() const { return openSong; }

    IntentResult run (const SongIntent& intent);
    // Shows the Song on the pages (the window's mode picks the visible one) and fills the top
    // bar's Song fields.
    void refresh (TopBarState& topBar);
    // Commits due arrow-key bursts and polls the Song (OpenSong::poll), presenting its Errors.
    void poll (song::SongPackage::Clock::time_point now);
    // One poll of the engine's peaks for the Mixer's and the Record rows' meters.
    void showMeters();
    // The peaks the meters show, as the last showMeters took them. Reading them takes no peak.
    const engine::MeterPeaks& shownMeters() const { return shownPeaks; }
    // Shows the frame heard `refreshNs` after `nowHostNs` on the Edit page's Video Preview and the
    // Record stage, each while it shows playback in its mode.
    void refreshVideo (std::uint64_t nowHostNs, std::uint64_t refreshNs);
    // Esc during a grip drag, and the focused fader's keys; true when the key was used.
    bool handleKey (const juce::KeyPress& key, song::SongPackage::Clock::time_point now);
    // A mouse down on `clicked`: a focused fader or pan elsewhere loses its focus.
    void releaseFocusOutside (const juce::Component* clicked);
    // The mode changed: no fader or pan keeps its focus.
    void releaseFocus();
    // Commits the Mixer's and the Record page's pending arrow-key bursts.
    void commitKeyBursts();
    // Starts converting the Song to `toRate` (the App group's convert_sample_rate, issue #160).
    IntentResult convertSampleRate (int toRate);
    // True while a Separation or a conversion runs: the window's modal run dialog shows.
    bool runShowing() const { return openSong.isSeparating() || openSong.isConverting(); }
    // Shows the running Separation's or conversion's progress on the dialog.
    void showRun (RunDialog& dialog) const;
    // True while the Export sheet shows (show_export). It is modal: it takes every click and key.
    bool isExportSheetShown() const { return exportSheetShown; }

    void resized() override;

    // Children are public so tests can click them and check what shows.
    RecordPage recordPage;
    EditPage editPage;
    ExportSheet exportSheet;
    PluginWindows pluginWindows;

private:
    void wirePages();

    // The Error the edit's Song operation or OpenSong handled when it was not applied.
    IntentResult applyEdit (core::Expected<song::Song> changed, const std::string& label);
    // A Mix's pending burst as its own undo step, through edit (not a nested run).
    void commitKeyBurst (MixFocus& mix);
    // The plain edit on the Song as it is now (ui/SongEdits.h), through applyEdit.
    IntentResult edit (const intent::Edit& edit);
    // A Song operation's Error (the Song as it is cannot take it), presented. Returns it.
    core::Error handleOperationError (const core::Error& error);
    // OpenSong's result, presented unless it is a refusal (MainComponent::handleUnlessRefused).
    IntentResult unlessRefused (const core::Expected<void>& result);
    // OpenSong's result, presented even when it is a refusal (MainComponent::handleError).
    IntentResult presented (const core::Expected<void>& result);

    IntentResult record();
    IntentResult undo();
    IntentResult redo();
    IntentResult seek (song::SampleCount sample);
    IntentResult jumpToPass (const song::Id& passId);
    IntentResult nudgePlayhead (Direction direction);
    // Adds an empty Track, Armed on the first input, as one undo step (issue #109).
    IntentResult addTrack();
    void askToImport();
    IntentResult importFiles (const std::vector<core::AbsolutePath>& files);
    // The × in an Edit Track header: asks, then deletes the Track as one undo step (issue #124).
    void askToDeleteTrack (const song::Id& trackId);
    IntentResult deleteTrack (const song::Id& trackId);
    IntentResult moveTrack (const song::Id& trackId, std::size_t toIndex);
    IntentResult setArmed (const song::Id& trackId, bool armed);
    IntentResult setInputChannels (const song::Id& trackId, std::vector<int> channels);
    IntentResult setMonitoring (const song::Id& trackId, bool on);
    IntentResult setPreRoll (song::PreRoll preRoll);
    IntentResult armCamera (bool armed);
    IntentResult cameraAccessAnswered (bool granted);
    IntentResult setCameraArmed (bool armed);
    IntentResult setCameraDevice (const std::string& cameraId);
    void showTrackMenu (const song::Id& trackId);
    IntentResult startSeparation (const song::Id& trackId);
    IntentResult cancelRun();
    IntentResult splitSelectedClip();
    IntentResult deleteSelection();
    IntentResult takeLaneClick (const song::Id& takeId, song::SampleCount at);
    IntentResult previewMix (const intent::PreviewMix& preview);
    IntentResult showExport (bool shown);
    // Shows or hides the Export sheet, logging ui.export_sheet_shown when that changes.
    void setExportSheetShown (bool shown);
    IntentResult startExport (const exporting::ExportRequest& request);
    IntentResult setExportVideoTake (const song::Id& videoTakeId);
    IntentResult revealExport();
    void chooseExportFile (const juce::File& current);
    void chooseInsert (const song::Id& trackId);
    IntentResult addInsert (const intent::AddInsert& insert);
    IntentResult addInsert (const song::Id& trackId, const plugin::AvailableAudioUnit& plugin);
    IntentResult openInsertEditor (const song::Id& slotId);
    IntentResult closePluginWindow (const song::Id& slotId);
    IntentResult clearClipIndicator (const song::Id& trackId);
    void syncPluginWindows();
    void handleErrors (const std::vector<core::Error>& errors);
    void takeKeyboardFocus();

    // A slot whose editor opens once its plugin is ready (just added, or clicked while loading).
    std::optional<song::Id> pendingEditorSlot;
    bool exportSheetShown = false;
    engine::MeterPeaks shownPeaks;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SongView)
};
} // namespace overdub::ui
