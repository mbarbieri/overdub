#pragma once

#include "core/Error.h"
#include "ui/Commands.h"
#include "ui/EditPage.h"
#include "ui/ExportPage.h"
#include "ui/InsertChain.h"
#include "ui/OpenSong.h"
#include "ui/PluginWindows.h"
#include "ui/RecordPage.h"
#include "ui/RunDialog.h"
#include "ui/TopBar.h"
#include "ui/TrackActions.h"

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

// The part of the main window that needs a Song: the Record, Edit and Export pages and the plugin
// editor windows, wired to one OpenSong. The window (ui/MainComponent.h) builds one when a Song
// opens and destroys it when the Song closes, so nothing in here sees a null Song. A callback
// that answers later (a chooser, a confirmation, camera access) holds a SafePointer to this
// object and is dropped once the Song has closed.
//
// `song` and `window` must outlive this object. Message thread only.
class SongView final : public juce::Component
{
    OpenSong& openSong;
    MainComponent& window;
    // Before the pages, which keep a reference to it.
    TrackActions trackActions;

public:
    SongView (OpenSong& song, MainComponent& window);
    ~SongView() override;

    OpenSong& currentSong() const { return openSong; }

    // Runs a command that acts on the Song (MainComponent::perform handles the mode commands).
    void perform (Command command);
    // Shows the Song on the pages (the window's mode picks the visible one) and fills the top
    // bar's Song fields.
    void refresh (TopBarState& topBar);
    // Commits due arrow-key bursts and polls the Song (OpenSong::poll), presenting its Errors.
    void poll (song::SongPackage::Clock::time_point now);
    // One poll of the engine's peaks for the Mixer's and the Record rows' meters.
    void showMeters();
    // Shows the frame heard `refreshNs` after `nowHostNs` on `preview` (when not null) and on the
    // Record stage when it shows playback.
    void refreshVideo (std::uint64_t nowHostNs, std::uint64_t refreshNs, VideoScreen* preview);
    // Esc during a grip drag, and the focused fader's keys; true when the key was used.
    bool handleKey (const juce::KeyPress& key, song::SongPackage::Clock::time_point now);
    // A mouse down on `clicked`: a focused fader or pan elsewhere loses its focus.
    void releaseFocusOutside (const juce::Component* clicked);
    // The mode changed: no fader or pan keeps its focus.
    void releaseFocus();
    // Commits the Mixer's and the Record page's pending arrow-key bursts.
    void commitKeyBursts();
    // Cancels the Separation or the conversion the run dialog shows.
    void cancelRun();
    void convertSampleRate (int toRate);
    // True while a Separation or a conversion runs: the window's modal run dialog shows.
    bool runShowing() const { return openSong.isSeparating() || openSong.isConverting(); }
    // Shows the running Separation's or conversion's progress on the dialog.
    void showRun (RunDialog& dialog) const;

    void resized() override;

    // Children are public so tests can click them and check what shows.
    RecordPage recordPage;
    EditPage editPage;
    ExportPage exportPage;
    PluginWindows pluginWindows;

private:
    TrackActions makeTrackActions();
    InsertChainActions makeInsertChainActions();
    void wirePages();

    void jumpToPass (const song::Id& passId);
    void splitSelectedClip();
    void deleteSelection();
    void nudgePlayhead (int seconds);
    // Whether the edit was applied; an Error is handled (a refusal is not presented).
    bool applyEdit (core::Expected<song::Song> changed, const std::string& label);
    // A Song operation's Error (the Song as it is cannot take it), unless the edit it was for is
    // refused anyway: then the refusal, not presented.
    void handleOperationError (const core::Error& error);
    void armCamera (bool armed);
    void setCameraArmed (bool armed);
    void setMute (const song::Id& trackId, bool mute);
    void setSolo (const song::Id& trackId, bool solo);
    // Adds an empty Track, Armed on the first input, as one undo step (issue #109).
    void addTrack();
    void askToImport();
    void importFiles (const std::vector<juce::File>& files);
    // The × in an Edit Track header: asks, then deletes the Track as one undo step (issue #124).
    void askToDeleteTrack (const song::Id& trackId);
    void deleteTrack (const song::Id& trackId);
    void moveTrack (const song::Id& trackId, std::size_t toIndex);
    void showTrackMenu (const song::Id& trackId);
    void startSeparation (const song::Id& trackId);
    void startExport (const exporting::ExportRequest& request);
    void chooseExportFile (const juce::File& current);
    void chooseInsert (const song::Id& trackId);
    void addInsert (const song::Id& trackId, const plugin::AvailableAudioUnit& plugin);
    void openInsertEditor (const song::Id& slotId);
    void syncPluginWindows();
    void handleErrors (const std::vector<core::Error>& errors);
    void takeKeyboardFocus();

    // A slot whose editor opens once its plugin is ready (just added, or clicked while loading).
    std::optional<song::Id> pendingEditorSlot;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SongView)
};
} // namespace overdub::ui
