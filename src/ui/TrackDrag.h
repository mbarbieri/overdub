#pragma once

#include "song/Song.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstddef>
#include <functional>
#include <optional>
#include <vector>

// Reordering Tracks by dragging their grip (issue #138), on the Edit page's Track headers and the
// Record page's rows. The grip is the only way to start a Track drag. While it drags, the dragged
// header or row follows the pointer at partial opacity and an accent line marks the drop slot
// (ui/Layout.h trackDropIndex); Esc cancels. The page reports a drop that changes the order as a
// Track id and target index for song::moveTrack. Transient UI state, never in the Song.
namespace overdub::ui
{
// The ⋮⋮ grip. Pressing, dragging and releasing it report the pointer in the grip's coordinates.
// Disabled (while a Pass runs) it ignores the mouse.
class TrackGrip final : public juce::Component
{
public:
    TrackGrip();

    std::function<void (juce::Point<int>)> onPress;
    std::function<void (juce::Point<int>)> onDrag;
    std::function<void (juce::Point<int>)> onRelease;

    void mouseDown (const juce::MouseEvent& event) override;
    void mouseDrag (const juce::MouseEvent& event) override;
    void mouseUp (const juce::MouseEvent& event) override;
    void enablementChanged() override { repaint(); }
    void paint (juce::Graphics& g) override;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TrackGrip)
};

// One page's Track drag, in that page's coordinates. `rows` are the Tracks' vertical spans in Song
// order, as trackDropIndex takes them.
class TrackDrag
{
public:
    struct Drop
    {
        song::Id trackId;
        std::size_t from = 0;
        std::size_t to = 0;
    };

    // Starts dragging the Track at index `from`, whose header or row is `row` and looks like
    // `ghost`, grabbed at pointerY.
    void begin (const song::Id& trackId, std::size_t from, juce::Rectangle<int> row, juce::Image ghost, int pointerY, const std::vector<juce::Range<int>>& rows);
    void moveTo (int pointerY, const std::vector<juce::Range<int>>& rows);
    // Ends the drag at pointerY: where it drops, or none when no drag was in progress (it was
    // cancelled, or never began).
    std::optional<Drop> end (int pointerY, const std::vector<juce::Range<int>>& rows);
    // Returns whether a drag was in progress.
    bool cancel();

    bool isActive() const { return active.has_value(); }
    // The dragged Track; none when no drag is in progress.
    std::optional<song::Id> trackId() const { return active.has_value() ? std::optional { active->trackId } : std::nullopt; }
    std::optional<std::size_t> dropIndex() const;
    // Where the dragged header or row is drawn; empty when no drag is in progress.
    juce::Rectangle<int> ghostBounds() const;
    // The insertion line's y; none when no drag is in progress.
    std::optional<int> dropLineY() const;

    // The ghost, and the insertion line from lineLeft to lineRight.
    void paint (juce::Graphics& g, int lineLeft, int lineRight) const;

private:
    struct Active
    {
        song::Id trackId;
        std::size_t from = 0;
        juce::Rectangle<int> row;
        juce::Image ghost;
        int grabOffset = 0; // the pointer's y within the row
        int pointerY = 0;
        std::size_t dropIndex = 0;
        int dropLineY = 0;
    };

    std::optional<Active> active;
};
} // namespace overdub::ui
