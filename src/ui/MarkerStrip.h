#pragma once

#include "song/Song.h"
#include "ui/Intent.h"
#include "ui/LoopViews.h"
#include "ui/TimelineGeometry.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

// The Loop/Marker strip (issues #227, #228, ADR 0010): a thin strip above the ruler in every mode
// that shows each Loop as a bar over its range (ui/LoopViews.h) and each Marker as a flag (a line
// at its position and its name) over the bars, and the Markers list of Transcribe mode's left
// column.
//
// Gestures, each an Intent through onIntent (ADR 0008): clicking a flag runs select_marker, which
// seeks to it (and selects it outside Record mode); clicking a bar runs select_loop. While the strip
// is editable (Transcribe and Edit), dragging a flag runs move_marker on release, dragging a bar's
// edge runs resize_loop on release, double-clicking a flag or a bar opens the name field, whose
// typed name runs rename_marker or rename_loop, dragging across the strip's empty space runs
// add_loop over that range on release, and clicking it runs select_marker with none. Read-only
// (Record mode), a drag changes nothing and there is no name field. A pending Loop start ([) shows
// as a line. A row of the list runs select_marker.
namespace overdub::ui
{
// The width of a flag beyond its name's: the line and the padding.
inline constexpr int markerFlagPadding = 12;

// How a strip maps Song samples to its own x: x = (sample - firstSample) / samplesPerPixel.
// Equality is exact: it compares the scale shown with the one asked for, never computed ones.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wfloat-equal"
struct StripScale
{
    double firstSample = 0.0;
    double samplesPerPixel = 1.0;

    bool operator== (const StripScale&) const = default;
};
#pragma clang diagnostic pop

// The scale of a timeline view whose firstSample sits at the strip's left edge.
StripScale stripScaleFor (const TimelineView& view);
// The scale that fits `length` samples into `width` pixels from 0 (Record's overview).
StripScale stripScaleFitting (song::SampleCount length, int width);
// The x of a Song sample on the strip (rounding down).
int stripX (const StripScale& scale, song::SampleCount sample);
// Where a flag dragged by `deltaX` pixels from `position` lands: 0 or later.
song::SampleCount draggedMarkerPosition (const StripScale& scale, song::SampleCount position, int deltaX);
// The Song sample at the strip's x: 0 or later.
song::SampleCount stripSample (const StripScale& scale, int x);
// The range a Loop's edge dragged by deltaX pixels gives it, as {start, end} in either order.
std::pair<song::SampleCount, song::SampleCount> resizedLoopRange (const StripScale& scale, const song::Loop& loop, LoopEdge edge, int deltaX);

// What the strip shows: the Song's Markers and Loops (time order) at a scale, whether it edits
// them, and the highlights.
struct StripState
{
    std::vector<song::Marker> markers;
    std::vector<song::Loop> loops;
    StripScale scale;
    bool editable = false;
    std::optional<song::Id> selectedMarker;
    std::optional<song::Id> selectedLoop;
    std::optional<song::Id> activeLoop;
    std::optional<song::SampleCount> pendingLoopStart;

    bool operator== (const StripState&) const = default;
};

// One Marker on the strip. Its left edge is the Marker's position.
class MarkerFlag final : public juce::Component
{
public:
    explicit MarkerFlag (song::Id markerId);

    const song::Id& markerId() const { return id; }
    const juce::String& name() const { return shownName; }
    bool isSelected() const { return selected; }
    void show (const song::Marker& marker, bool isSelected);
    void paint (juce::Graphics& g) override;
    void mouseDown (const juce::MouseEvent& event) override;
    void mouseDrag (const juce::MouseEvent& event) override;
    void mouseUp (const juce::MouseEvent& event) override;
    void mouseDoubleClick (const juce::MouseEvent& event) override;

    // A click without a drag; a drag by deltaX pixels in progress, and released.
    std::function<void()> onClick;
    std::function<void (int deltaX)> onDrag;
    std::function<void (int deltaX)> onDragEnd;
    std::function<void()> onDoubleClick;

private:
    song::Id id;
    juce::String shownName;
    bool selected = false;
    bool dragging = false;
};

class MarkerStrip final : public juce::Component
{
public:
    MarkerStrip();

    void show (const StripState& state);
    bool isEditable() const { return shown.editable; }
    // The range an empty-space drag in progress would make a Loop of; none otherwise.
    std::optional<std::pair<song::SampleCount, song::SampleCount>> draggedRange() const;
    void paint (juce::Graphics& g) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent& event) override;
    void mouseDrag (const juce::MouseEvent& event) override;
    void mouseUp (const juce::MouseEvent& event) override;

    IntentSink onIntent;

    // Children are public so tests can click them (ADR 0005 UI amendment).
    std::vector<std::unique_ptr<LoopBar>> bars;
    std::vector<std::unique_ptr<MarkerFlag>> flags;
    // Over the double-clicked flag or bar while its name is edited; hidden otherwise.
    juce::Label nameField;

private:
    // What the name field renames.
    struct Renaming
    {
        song::Id id;
        bool isLoop = false;
    };

    void emit (const Intent& intent) const;
    void showFlags();
    void showBars();
    void placeFlag (MarkerFlag& flag, song::SampleCount position);
    void placeBar (LoopBar& bar, song::SampleCount start, song::SampleCount end);
    void startRenaming (Renaming target, juce::Rectangle<int> over, const std::string& name);
    const song::Marker* markerWithId (const song::Id& markerId) const;
    const song::Loop* loopWithId (const song::Id& loopId) const;

    StripState shown;
    std::optional<Renaming> renaming;
    // An empty-space drag: where it went down and where it is now (strip x).
    std::optional<int> dragDownX;
    int dragX = 0;
    bool draggingRange = false;
};

// One row of the Markers list: the Marker's name and its time.
class MarkerRow final : public juce::Component
{
public:
    explicit MarkerRow (song::Id markerId);

    const song::Id& markerId() const { return id; }
    void show (const song::Marker& marker, int sampleRate, bool isSelected);
    void paint (juce::Graphics& g) override;
    void mouseUp (const juce::MouseEvent& event) override;

    std::function<void()> onClick;

private:
    song::Id id;
    juce::String name;
    juce::String time;
    bool selected = false;
};

// Transcribe mode's left column (issue #227): "MARKERS" over one MarkerRow per Marker in time
// order, or a hint while the Song has none.
class MarkerList final : public juce::Component
{
public:
    MarkerList();

    void show (const std::vector<song::Marker>& markers, int sampleRate, std::optional<song::Id> selected);
    void resized() override;
    void paint (juce::Graphics& g) override;

    IntentSink onIntent;

    juce::Label heading { {}, "MARKERS" };
    juce::Label hint;
    std::vector<std::unique_ptr<MarkerRow>> rows;

private:
    void layOutRows();
};
} // namespace overdub::ui
