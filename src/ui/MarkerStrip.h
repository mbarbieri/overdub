#pragma once

#include "song/Song.h"
#include "ui/Intent.h"
#include "ui/TimelineGeometry.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

// The Loop/Marker strip (issue #227, ADR 0010): a thin strip above the ruler in every mode that
// shows each Marker as a flag (a line at its position and its name), and the Markers list of
// Transcribe mode's left column.
//
// Gestures, each an Intent through onIntent (ADR 0008): clicking a flag runs select_marker, which
// seeks to it (and selects it outside Record mode). While the strip is editable (Transcribe and
// Edit), dragging a flag runs move_marker on release, double-clicking it opens the name field,
// whose typed name runs rename_marker, and clicking the strip's empty space runs select_marker
// with none. Read-only (Record mode), a drag moves nothing and there is no name field. A row of
// the list runs select_marker.
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

    // The Markers in Song order (time order), at this scale; `selected` is highlighted.
    void show (const std::vector<song::Marker>& markers, StripScale scale, bool editable, std::optional<song::Id> selected);
    bool isEditable() const { return editable; }
    void paint (juce::Graphics& g) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent& event) override;

    IntentSink onIntent;

    // Children are public so tests can click them (ADR 0005 UI amendment).
    std::vector<std::unique_ptr<MarkerFlag>> flags;
    // Over the double-clicked flag while its name is edited; hidden otherwise.
    juce::Label nameField;

private:
    void emit (const Intent& intent) const;
    void showFlags();
    void placeFlag (MarkerFlag& flag, song::SampleCount position);
    void startRenaming (const song::Id& markerId);
    const song::Marker* markerWithId (const song::Id& markerId) const;

    std::vector<song::Marker> markers;
    StripScale scale;
    bool editable = false;
    std::optional<song::Id> selected;
    std::optional<song::Id> renaming;
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
