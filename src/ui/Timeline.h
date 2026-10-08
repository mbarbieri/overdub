#pragma once

#include "song/Peaks.h"
#include "song/Song.h"
#include "ui/Layout.h"
#include "ui/TakeLanes.h"
#include "ui/TimelineGeometry.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

// Edit mode's timeline (issue #28, issue #9 resolution, variant C): a ruler, the Camera lane
// (a placeholder until slice 15), and one lane per Track with a compact header (arm, name). Clips
// are drawn in layer order with waveforms from the peaks cache, a hatched strip marks where a
// Clip covers an older one, and the playhead crosses every lane.
//
// Each Clip shows its fades (shaded, with a line from silence to full level), a fade handle at each
// fade's inner end in its top row, and its gain as a dB tag in its top-right corner (issue #29).
//
// Gestures (thin over ui/TimelineGeometry.h): clicking a Clip selects it, dragging its body moves
// it, dragging an edge trims it, dragging a fade handle sets that fade, dragging the dB tag up or
// down sets the gain; the drag is previewed and reported once, on mouse up, as one Song and its
// undo label. Clicking empty lane space clears the selection; clicking or dragging
// in the ruler moves the playhead. The scroll bar and the mouse wheel scroll; zoomIn/zoomOut zoom.
// Selection and the view are transient UI state, never in the Song.
//
// Take Lanes (issue #30): a Track header with Takes shows a "▸ n Takes" toggle that unfolds one
// thin lane per Take recorded on the Track, newest Pass first (ui/TakeLanes.h): the Take's
// recorded span as a block, the spans where it is heard now filled in, and its heard status in
// the lane's header. Clicking a lane at a point swaps the top Clip there to that Take, or brings
// the whole Take back on top (laneEditAt), reported through onEdit with its label; the swapped or
// added Clip is selected. Which Tracks are unfolded is transient UI state, like selection.
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
};

// The name a Clip shows: "Pass n" for a Take (its Pass's number), the file name for a Backing
// Track, "Missing" when the Song lacks its source.
std::string clipName (const song::Song& song, const song::Clip& clip);

// A Track lane's compact header: arm and name, the Take Lanes toggle when it has Takes, and M and S
// (issue #31).
class TrackHeader final : public juce::Component
{
public:
    TrackHeader();

    // numTakes: the Takes recorded on the Track (no toggle when 0); unfolded: its lanes are shown.
    void show (const song::Track& track, bool enabled, int numTakes, bool unfolded);
    void resized() override;
    void paint (juce::Graphics& g) override;

    const song::Id& trackId() const { return id; }

    std::function<void (bool armed)> onArm;
    std::function<void()> onToggleTakes;
    std::function<void (bool mute)> onMute;
    std::function<void (bool solo)> onSolo;

    juce::TextButton armButton { "Arm" };
    juce::Label nameLabel;
    juce::TextButton takesButton;
    juce::TextButton muteButton { "M" };
    juce::TextButton soloButton { "S" };

private:
    song::Id id;
    bool armed = false;
    bool muted = false;
    bool soloed = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TrackHeader)
};

class Timeline final : public juce::Component, private juce::ScrollBar::Listener
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

    const std::optional<song::Id>& selectedClip() const { return selected; }
    void selectClip (std::optional<song::Id> clipId);

    // Where a Clip of the shown Song is drawn, in this Component's coordinates (cut to its lane);
    // empty when it is not on screen.
    juce::Rectangle<int> clipBounds (const song::Id& clipId) const;
    // Where a Clip's fade handle or dB tag (a handle part) is drawn, cut to the lanes; empty when it
    // is not on screen, or for another part.
    juce::Rectangle<int> handleBounds (const song::Id& clipId, ClipPart part) const;
    juce::Rectangle<int> rulerBounds() const { return layout.ruler; }

    // Take Lanes: unfolding is per Track and transient.
    void setTakeLanesUnfolded (const song::Id& trackId, bool unfolded);
    bool areTakeLanesUnfolded (const song::Id& trackId) const { return unfoldedTracks.contains (trackId); }
    // The lanes shown under this Track (empty when folded), newest Pass first.
    std::vector<TakeLaneRow> takeLanesShown (const song::Id& trackId) const;
    // Where the lane of this Take is drawn, cut to the lanes; empty when folded or off screen.
    juce::Rectangle<int> takeLaneBounds (const song::Id& takeId) const;
    juce::Rectangle<int> cameraLaneBounds() const { return layout.cameraLane; }

    // A finished drag or lane click: the changed Song and its undo label ("Move Clip", "Trim Clip",
    // "Fade In", "Fade Out", "Clip Gain", "Use Pass 2 on Guitar", "Bring back Pass 2 on Guitar").
    std::function<void (song::Song changed, std::string label)> onEdit;
    std::function<void (song::SampleCount sample)> onSeek;
    std::function<void (const song::Id& trackId, bool armed)> onArm;
    std::function<void (const song::Id& trackId, bool mute)> onMute;
    std::function<void (const song::Id& trackId, bool solo)> onSolo;
    std::function<void()> onSelectionChanged;
    PeaksLookup peaksFor;

    void mouseDown (const juce::MouseEvent& event) override;
    void mouseDrag (const juce::MouseEvent& event) override;
    void mouseUp (const juce::MouseEvent& event) override;
    void mouseWheelMove (const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override;
    void resized() override;
    void paint (juce::Graphics& g) override;

    std::vector<std::unique_ptr<TrackHeader>> headers;
    juce::Label cameraHeader;
    juce::ScrollBar scrollBar { false };

private:
    struct ActiveDrag
    {
        ClipDrag drag;
        song::Song songAtDown;
    };

    void scrollBarMoved (juce::ScrollBar* bar, double newRangeStart) override;
    void rebuildHeaders();
    void showHeaders();
    void refreshTakeLanes();
    bool clickTakeLane (juce::Point<int> position);
    void updateScrollBar();
    const song::Song& shownSong() const { return preview.has_value() ? *preview : current.song; }
    std::optional<std::size_t> laneAt (juce::Point<int> position) const;
    juce::Rectangle<int> lanesArea() const;

    void paintRuler (juce::Graphics& g) const;
    void paintCameraLane (juce::Graphics& g) const;
    void paintLane (juce::Graphics& g, const song::Track& track, juce::Rectangle<int> lane) const;
    void paintClip (juce::Graphics& g, const song::Clip& clip, juce::Rectangle<int> clipArea) const;
    void paintFadesAndGain (juce::Graphics& g, const song::Clip& clip, juce::Rectangle<int> clipArea) const;
    void paintCoveredSpans (juce::Graphics& g, const song::Track& track, juce::Rectangle<int> lane) const;
    void paintPlayhead (juce::Graphics& g) const;
    void paintTakeLane (juce::Graphics& g, const TakeLaneRow& row, juce::Rectangle<int> lane) const;
    void paintTakeLaneHeader (juce::Graphics& g, const TakeLaneRow& row, juce::Rectangle<int> header) const;

    TimelineState current;
    std::optional<song::Song> preview; // the Song a drag in progress would make
    std::optional<ActiveDrag> activeDrag;
    bool seeking = false; // a ruler drag in progress
    TimelineView timelineView;
    TimelineLayout layout;
    std::optional<song::Id> selected;
    std::set<song::Id> unfoldedTracks;
    std::vector<std::vector<TakeLaneRow>> takeLanes; // per Track of the shown Song; empty when folded

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Timeline)
};
} // namespace overdub::ui
