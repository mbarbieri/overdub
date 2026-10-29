#pragma once

#include "song/Peaks.h"
#include "song/Song.h"
#include "ui/Intent.h"
#include "ui/Layout.h"
#include "ui/MarkerStrip.h"
#include "ui/PeaksLookup.h"
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
#include <string_view>
#include <variant>
#include <vector>

// Edit mode's timeline (issue #28, issue #9 resolution, variant C): a ruler, the Camera lane
// (issue #36: one block per Video Take, from its start to the end of its movie on the audio clock,
// titled with its Pass; a Video Take whose movie cannot be read is a narrow block; its header's A
// arms the camera, issue #198), and one lane
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
// down sets the gain; the drag is previewed and reported once, on mouse up, as its plain edit
// (intentForDrag). Clicking a Video Take's block in the Camera lane selects it (issue #128; it
// cannot be dragged). Clicking empty lane space clears the selection; clicking or dragging in the
// ruler moves the playhead. The scroll bars and the mouse wheel scroll, and a trackpad pinch zooms
// around the pointer (issue #164, pinchZoomed). Selection and the view are transient UI state,
// never in the Song.
//
// Every gesture that changes state is reported as an Intent through onIntent (ADR 0008), which
// runs it: select_clip, select_video_take, seek, set_timeline_view, set_vertical_scroll,
// set_take_lanes_unfolded, take_lane_click, move_track, import_files, set_armed, set_camera_armed, the Track
// header's mute and solo, and a drag's plain edit. The Timeline changes what it shows only when
// the Intent's handler calls back (selectClip, setView, ...); gesture phases (a drag's preview,
// the pinch's pending levels, Esc) stay here.
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
// the whole Take back on top (laneEditAt), reported as take_lane_click, whose handler selects the
// swapped or added Clip. Which Tracks are unfolded is transient UI state, like selection.
//
// Dropping files (issue #123): a drag holding at least one importable audio file (ui/FileDrop.h)
// is accepted while canImportFiles says so, highlights the Timeline while it hovers, and on drop
// reports the importable files as import_files. Where they are dropped does not matter.
//
// Reordering Tracks (issue #138, ui/TrackDrag.h): dragging a header's grip drags its Track, with
// its unfolded Take Lanes, between the other Tracks (never above the Camera lane); a drop that
// changes the order is reported as move_track. No grip drags while a Pass runs.
//
// One gesture at a time (issue #148): a Clip drag, a ruler drag (seek), a grip drag, or a file drag
// hovering. While one is in progress the others do not start. Esc (cancelGesture) ends a Clip drag
// (its preview dropped, no edit reported), a ruler drag (the playhead stays where it was sought)
// or a grip drag (no drop); a file drag is the system's and carries on. A Pass starting ends a grip
// drag, and so does its Track leaving the Song. A Clip drag carries on over a Song changed by
// anything else (issue #157): its edit, measured from the Clip at mouse down, previews on and
// commits to the current Song; its Clip leaving the Song ends it. The gesture handling is in
// ui/TimelineGestures.cpp.
namespace overdub::ui
{
struct TimelineState
{
    song::Song song;
    song::SampleCount playhead = 0;
    bool playing = false;
    bool editable = true; // false while a Pass runs: no gestures
    bool tracksDeletable = true; // false while a Pass, a Separation or an Export runs
    std::vector<video::VideoTakeSpan> videoTakes; // the Camera lane's blocks (video::videoTakeSpans)
    bool canArmCamera = true; // the Camera lane header's A: as Record mode's camera arm, and not refused
    // The Loop/Marker strip (issue #227): the selected Marker, and whether the strip edits Markers.
    std::optional<song::Id> selectedMarker;
    bool markersEditable = true;
    // The Loop selected in Edit mode and the pending Loop start ([), issue #228.
    std::optional<song::Id> selectedLoop;
    std::optional<song::SampleCount> pendingLoopStart;
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

    // numTakes: the Takes recorded on the Track (no toggle when 0); unfolded: its lanes are shown.
    void show (const song::Track& track, bool enabled, bool deletable, int numTakes, bool unfolded);
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
    explicit Timeline (PeaksLookup peaksFor);
    ~Timeline() override;

    void show (const TimelineState& state);

    const TimelineView& view() const { return timelineView; }
    // Shows the Song from this sample at this zoom (clamped to the limits).
    void setView (song::SampleCount firstSample, song::SampleCount samplesPerPixel);

    // Selection is single: at most one Clip or one Video Take. Selecting either clears the other;
    // selectClip (std::nullopt) clears both.
    std::optional<song::Id> selectedClip() const;
    void selectClip (std::optional<song::Id> clipId);
    std::optional<song::Id> selectedVideoTake() const;
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
    // The Song drawn: a Clip drag's preview while one has moved, else the current Song.
    const song::Song& shownSong() const;
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

    IntentSink onIntent;
    // The × in a Track's header was clicked (the window asks before deleting).
    std::function<void (const song::Id& trackId)> onDeleteTrack;
    std::function<void()> onSelectionChanged;
    // Whether a drop would be imported now (false while the Song is locked); no callback = never.
    std::function<bool()> canImportFiles;

    // The grip drag in progress, if any (inactive otherwise); valid until the gesture changes.
    const TrackDrag& trackDrag() const;
    // Ends the Clip drag, ruler drag or grip drag in progress, reporting nothing more, and returns
    // whether there was one. A file drag hovering is left alone. reason is logged ("escape",
    // "pass_running", "track_removed").
    bool cancelGesture (std::string_view reason);
    // Each Track's row (its lane and unfolded Take Lanes) as a vertical span in this Component's
    // coordinates, in Song order.
    std::vector<juce::Range<int>> trackRowSpans() const;

    bool isDropHighlighted() const { return std::holds_alternative<FileDragHover> (gesture); }
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void fileDragEnter (const juce::StringArray& files, int x, int y) override;
    void fileDragExit (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

    void mouseDown (const juce::MouseEvent& event) override;
    void mouseDrag (const juce::MouseEvent& event) override;
    void mouseUp (const juce::MouseEvent& event) override;
    void mouseWheelMove (const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override;
    void mouseMagnify (const juce::MouseEvent& event, float scaleFactor) override;
    void resized() override;
    void paint (juce::Graphics& g) override;
    void paintOverChildren (juce::Graphics& g) override;

    std::vector<std::unique_ptr<TrackHeader>> headers; // children of trackHeaderColumn
    juce::Label cameraHeader;
    juce::TextButton cameraArmButton { "A" };
    juce::ScrollBar scrollBar { false };
    juce::ScrollBar verticalScrollBar { true }; // shown only when the Track rows overflow
    MarkerStrip markerStrip; // above the ruler, at the timeline's view (issue #227)

private:
    struct ClipSelection
    {
        song::Id clipId;
        bool operator== (const ClipSelection&) const = default;
    };
    struct VideoTakeSelection
    {
        song::Id videoTakeId;
        bool operator== (const VideoTakeSelection&) const = default;
    };
    using Selection = std::variant<std::monostate, ClipSelection, VideoTakeSelection>;

    struct ClipDragGesture
    {
        ClipDrag drag;
        song::Song songAtDown;
        std::optional<intent::Edit> edit; // what its release commits, once it has moved
        std::optional<song::Song> preview; // editedSong (the current Song, edit)
    };
    struct SeekGesture
    {
    };
    struct FileDragHover
    {
    };
    // TrackDrag is the grip drag, always active while it is the gesture.
    using Gesture = std::variant<std::monostate, ClipDragGesture, SeekGesture, TrackDrag, FileDragHover>;

    void scrollBarMoved (juce::ScrollBar* bar, double newRangeStart) override;
    void emit (Intent intent) const;
    void setSelection (Selection next);
    bool isSelectedClip (const song::Id& clipId) const;
    // A Clip's rectangle in its lane: its columns, inset 3 pixels from the lane's top and bottom.
    static juce::Rectangle<int> clipRectangle (juce::Rectangle<int> lane, ClipColumns columns);
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
    void previewClipDrag (ClipDragGesture& clipDrag) const;
    void layOutRows();
    void showMarkerStrip();
    void revealNewTracks (const std::vector<song::Id>& previousTrackIds);
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

    PeaksLookup peaksFor;
    TimelineState current;
    Gesture gesture;
    TimelineView timelineView;
    int verticalScrollPixels = 0;
    double pinchPendingLevels = 0.0; // PinchZoom::pendingLevels between pinch events
    // True while the horizontal scroll bar reports a move: it already shows that range, and its
    // limits stay as they are while its thumb is dragged. The periodic refresh (show) can still
    // update the scroll bar while playing, as it could before set_timeline_view was an Intent.
    bool scrollBarReporting = false;
    TimelineLayout layout;
    juce::Component trackHeaderColumn; // the tracks viewport's header column: cuts off scrolled headers
    Selection selection;
    std::set<song::Id> unfoldedTracks;
    std::vector<std::vector<TakeLaneRow>> takeLanes; // per Track of the shown Song; empty when folded

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Timeline)
};
} // namespace overdub::ui
