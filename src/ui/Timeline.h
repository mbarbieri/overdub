#pragma once

#include "song/Peaks.h"
#include "song/Song.h"
#include "ui/Layout.h"
#include "ui/TakeLanes.h"
#include "ui/TimelineGeometry.h"
#include "ui/TrackDrag.h"
#include "video/FrameSelection.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

// Edit mode's timeline (issue #28, issue #9 resolution, variant C): a ruler, the Camera lane
// (issue #36: one block per Video Take, from its start to the end of its movie on the audio clock,
// titled with its Pass; a Video Take whose movie cannot be read is a narrow block), and one lane
// per Track with a compact header (its colour stripe, grip, name, Take Lanes toggle, A M S, ×). Clips are
// drawn in their Track's colour (issue #137), in layer order with waveforms from
// the peaks cache, a hatched strip marks where a Clip covers an older one, and the playhead
// crosses every lane.
//
// Each Clip shows its fades (shaded, with a line from silence to full level), a fade handle at each
// fade's inner end in its top row, and its gain as a dB tag in its top-right corner (issue #29).
//
// Gestures (thin over ui/TimelineGeometry.h): clicking a Clip selects it, dragging its body moves
// it, dragging an edge trims it, dragging a fade handle sets that fade, dragging the dB tag up or
// down sets the gain; the drag is previewed and reported once, on mouse up, as one Song and its
// undo label. Clicking a Video Take's block in the Camera lane selects it (issue #128; it cannot be
// dragged). Clicking empty lane space clears the selection; clicking or dragging
// in the ruler moves the playhead. The scroll bar and the mouse wheel scroll; zoomIn/zoomOut zoom.
// Selection and the view are transient UI state, never in the Song.
//
// Vertical scrolling (issue #127): when the Track rows are taller than the space under the Camera
// lane, they scroll under the fixed ruler and Camera lane, with a vertical scroll bar at the right
// (ui/Layout.h). A vertical wheel or swipe scrolls them, a horizontal one (or shift with the wheel)
// scrolls time (wheelScroll). The view moves on its own only to reveal Tracks the Song did not
// have before (an Import, a Drop, a Separation, an undone delete); the scroll is clamped whenever
// the rows or the Timeline change size.
//
// Take Lanes (issue #30): a Track header with Takes shows a "▸ n Takes" toggle that unfolds one
// thin lane per Take recorded on the Track, newest Pass first (ui/TakeLanes.h): the Take's
// recorded span as a block, the spans where it is heard now filled in, and its heard status in
// the lane's header. Clicking a lane at a point swaps the top Clip there to that Take, or brings
// the whole Take back on top (laneEditAt), reported through onEdit with its label; the swapped or
// added Clip is selected. Which Tracks are unfolded is transient UI state, like selection.
//
// Dropping files (issue #123): a drag holding at least one importable audio file (ui/FileDrop.h)
// is accepted while canImportFiles says so, highlights the Timeline while it hovers, and on drop
// reports the importable files through onFilesDropped. Where they are dropped does not matter.
//
// Reordering Tracks (issue #138, ui/TrackDrag.h): dragging a header's grip drags its Track, with
// its unfolded Take Lanes, between the other Tracks (never above the Camera lane); a drop that
// changes the order is reported through onMoveTrack. No grip drags while a Pass runs.
namespace overdub::ui
{
// The peaks of a Clip's source (no channels: draw no waveform). The reference must stay valid
// while the Timeline paints.
using PeaksLookup = std::function<const song::Peaks&(const song::ClipSource&)>;

struct TimelineState
{
    song::Song song;
    song::SampleCount playhead = 0;
    bool playing = false;
    bool editable = true; // false while a Pass runs: no gestures
    bool tracksDeletable = true; // false while a Pass, a Separation or an Export runs
    std::vector<video::VideoTakeSpan> videoTakes; // the Camera lane's blocks (video::videoTakeSpans)
};

// The name a Clip shows: "Pass n" for a Take (its Pass's number), the file name for a Backing
// Track, "Missing" when the Song lacks its source.
std::string clipName (const song::Song& song, const song::Clip& clip);

// A Track lane's compact header: the grip (issue #138) and name, the Take Lanes toggle when it has
// Takes, A, M and S (issue #31), and × to delete the Track (issue #124).
class TrackHeader final : public juce::Component
{
public:
    TrackHeader();

    // trackArmed: the Track's Armed setting; numTakes: the Takes recorded on the Track (no toggle
    // when 0); unfolded: its lanes are shown.
    void show (const song::Track& track, bool trackArmed, bool enabled, bool deletable, int numTakes, bool unfolded);
    void resized() override;
    void paint (juce::Graphics& g) override;

    const song::Id& trackId() const { return id; }
    // The Track's colour (ui/Colours.h), drawn as a stripe along the header's left edge.
    juce::Colour colour() const { return stripeColour; }

    std::function<void (bool armed)> onArm;
    std::function<void()> onToggleTakes;
    std::function<void (bool mute)> onMute;
    std::function<void (bool solo)> onSolo;
    std::function<void()> onDelete;

    TrackGrip grip;
    juce::TextButton armButton { "A" };
    juce::Label nameLabel;
    juce::TextButton takesButton;
    juce::TextButton muteButton { "M" };
    juce::TextButton soloButton { "S" };
    juce::TextButton deleteButton { juce::String::fromUTF8 ("\u00d7") };

private:
    song::Id id;
    juce::Colour stripeColour;
    bool armed = false;
    bool muted = false;
    bool soloed = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TrackHeader)
};

class Timeline final : public juce::Component, public juce::FileDragAndDropTarget, private juce::ScrollBar::Listener
{
public:
    Timeline();
    ~Timeline() override;

    void show (const TimelineState& state);

    const TimelineView& view() const { return timelineView; }
    // Shows the Song from this sample at this zoom (clamped to the limits).
    void setView (song::SampleCount firstSample, song::SampleCount samplesPerPixel);
    void zoomIn();
    void zoomOut();

    // Selection is single: at most one Clip or one Video Take. Selecting either clears the other;
    // selectClip (std::nullopt) clears both.
    const std::optional<song::Id>& selectedClip() const { return selected; }
    void selectClip (std::optional<song::Id> clipId);
    const std::optional<song::Id>& selectedVideoTake() const { return selectedVideo; }
    void selectVideoTake (std::optional<song::Id> videoTakeId);

    // Where a Clip of the shown Song is drawn, in this Component's coordinates (cut to its lane);
    // empty when it is not on screen.
    juce::Rectangle<int> clipBounds (const song::Id& clipId) const;
    // The colour a Clip of the shown Song is drawn in: its Track's (ui/Colours.h); none when no
    // Track holds it.
    std::optional<juce::Colour> clipColour (const song::Id& clipId) const;
    // Where a Clip's fade handle or dB tag (a handle part) is drawn, cut to the lanes; empty when it
    // is not on screen, or for another part.
    juce::Rectangle<int> handleBounds (const song::Id& clipId, ClipPart part) const;
    juce::Rectangle<int> rulerBounds() const { return layout.ruler; }
    // Where Track rows show (header column and lanes), under the Camera lane.
    juce::Rectangle<int> tracksViewportBounds() const { return layout.tracksViewport; }

    // How far the Track rows are scrolled up, in pixels; set clamped to what the rows allow.
    int verticalScroll() const { return verticalScrollPixels; }
    void setVerticalScroll (int pixels);

    // Take Lanes: unfolding is per Track and transient.
    void setTakeLanesUnfolded (const song::Id& trackId, bool unfolded);
    bool areTakeLanesUnfolded (const song::Id& trackId) const { return unfoldedTracks.contains (trackId); }
    // The lanes shown under this Track (empty when folded), newest Pass first.
    std::vector<TakeLaneRow> takeLanesShown (const song::Id& trackId) const;
    // Where the lane of this Take is drawn, cut to the lanes; empty when folded or off screen.
    juce::Rectangle<int> takeLaneBounds (const song::Id& takeId) const;
    // The colour of the lane of this Take: its Track's, dimmed (takeLaneColour); none when folded.
    std::optional<juce::Colour> takeLaneColourOf (const song::Id& takeId) const;
    juce::Rectangle<int> cameraLaneBounds() const { return layout.cameraLane; }
    // Where a Video Take's block is drawn in the Camera lane, cut to it; empty when off screen or
    // not shown.
    juce::Rectangle<int> videoTakeBounds (const song::Id& videoTakeId) const;
    static constexpr int unreadableVideoTakeWidth = 6;

    // A finished drag or lane click: the changed Song and its undo label ("Move Clip", "Trim Clip",
    // "Fade In", "Fade Out", "Clip Gain", "Use Pass 2 on Guitar", "Bring back Pass 2 on Guitar").
    std::function<void (song::Song changed, std::string label)> onEdit;
    std::function<void (song::SampleCount sample)> onSeek;
    std::function<void (const song::Id& trackId, bool armed)> onArm;
    std::function<void (const song::Id& trackId, bool mute)> onMute;
    std::function<void (const song::Id& trackId, bool solo)> onSolo;
    // The × in a Track's header was clicked (the window asks before deleting).
    std::function<void (const song::Id& trackId)> onDeleteTrack;
    // A grip drag dropped the Track at this index of the Track order (a different one).
    std::function<void (const song::Id& trackId, std::size_t toIndex)> onMoveTrack;
    std::function<void()> onSelectionChanged;
    PeaksLookup peaksFor;
    // Whether a drop would be imported now (false while the Song is locked); no callback = never.
    std::function<bool()> canImportFiles;
    // The importable files of a drop, in drop order.
    std::function<void (std::vector<juce::File> files)> onFilesDropped;

    // The grip drag in progress, if any; cancelTrackDrag ends it without a drop and returns whether
    // there was one (Esc).
    const TrackDrag& trackDrag() const { return activeTrackDrag; }
    bool cancelTrackDrag();
    // Each Track's row (its lane and unfolded Take Lanes) as a vertical span in this Component's
    // coordinates, in Song order.
    std::vector<juce::Range<int>> trackRowSpans() const;

    bool isDropHighlighted() const { return dropHighlighted; }
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void fileDragEnter (const juce::StringArray& files, int x, int y) override;
    void fileDragExit (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

    void mouseDown (const juce::MouseEvent& event) override;
    void mouseDrag (const juce::MouseEvent& event) override;
    void mouseUp (const juce::MouseEvent& event) override;
    void mouseWheelMove (const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override;
    void resized() override;
    void paint (juce::Graphics& g) override;
    void paintOverChildren (juce::Graphics& g) override;

    std::vector<std::unique_ptr<TrackHeader>> headers; // children of trackHeaderColumn
    juce::Label cameraHeader;
    juce::ScrollBar scrollBar { false };
    juce::ScrollBar verticalScrollBar { true }; // shown only when the Track rows overflow

private:
    struct ActiveDrag
    {
        ClipDrag drag;
        song::Song songAtDown;
    };

    void scrollBarMoved (juce::ScrollBar* bar, double newRangeStart) override;
    void setSelection (std::optional<song::Id> clipId, std::optional<song::Id> videoTakeId);
    // The Video Take whose block is at this point, the one drawn on top where blocks overlap.
    std::optional<song::Id> videoTakeAt (juce::Point<int> position) const;
    void rebuildHeaders();
    void connectHeader (TrackHeader& header, const song::Id& trackId);
    void beginTrackDrag (TrackHeader& header, juce::Point<int> inGrip);
    void dragTrack (TrackHeader& header, juce::Point<int> inGrip);
    void dropTrack (TrackHeader& header, juce::Point<int> inGrip);
    void showHeaders();
    void refreshTakeLanes();
    bool clickTakeLane (juce::Point<int> position);
    void updateScrollBar();
    void layOutRows();
    void revealNewTracks (const std::vector<song::Id>& previousTrackIds);
    const song::Song& shownSong() const { return preview.has_value() ? *preview : current.song; }
    std::optional<std::size_t> laneAt (juce::Point<int> position) const;
    juce::Rectangle<int> lanesArea() const;

    void paintRuler (juce::Graphics& g) const;
    void paintCameraLane (juce::Graphics& g) const;
    void paintLane (juce::Graphics& g, const song::Track& track, juce::Rectangle<int> lane) const;
    void paintClip (juce::Graphics& g, const song::Clip& clip, juce::Colour body, juce::Rectangle<int> clipArea) const;
    void paintFadesAndGain (juce::Graphics& g, const song::Clip& clip, juce::Rectangle<int> clipArea) const;
    void paintCoveredSpans (juce::Graphics& g, const song::Track& track, juce::Rectangle<int> lane) const;
    void paintPlayhead (juce::Graphics& g) const;
    void paintTakeLane (juce::Graphics& g, const TakeLaneRow& row, juce::Colour colour, juce::Rectangle<int> lane) const;
    void paintTakeLaneHeader (juce::Graphics& g, const TakeLaneRow& row, juce::Rectangle<int> header) const;

    TimelineState current;
    std::optional<song::Song> preview; // the Song a drag in progress would make
    std::optional<ActiveDrag> activeDrag;
    bool seeking = false; // a ruler drag in progress
    bool dropHighlighted = false; // an accepted file drag hovers
    TrackDrag activeTrackDrag;
    TimelineView timelineView;
    int verticalScrollPixels = 0;
    TimelineLayout layout;
    juce::Component trackHeaderColumn; // the tracks viewport's header column: cuts off scrolled headers
    std::optional<song::Id> selected;      // a Clip
    std::optional<song::Id> selectedVideo; // a Video Take; never set together with `selected`
    std::set<song::Id> unfoldedTracks;
    std::vector<std::vector<TakeLaneRow>> takeLanes; // per Track of the shown Song; empty when folded

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Timeline)
};
} // namespace overdub::ui
