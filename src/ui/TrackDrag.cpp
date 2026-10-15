#include "ui/TrackDrag.h"

#include "ui/Colours.h"
#include "ui/Layout.h"

namespace overdub::ui
{
namespace
{
constexpr float ghostOpacity = 0.6f;
constexpr int dropLineThickness = 2;
} // namespace

TrackGrip::TrackGrip()
{
    setMouseCursor (juce::MouseCursor::DraggingHandCursor);
}

void TrackGrip::mouseDown (const juce::MouseEvent& event)
{
    if (isEnabled() && onPress)
        onPress (event.getPosition());
}

void TrackGrip::mouseDrag (const juce::MouseEvent& event)
{
    if (isEnabled() && onDrag)
        onDrag (event.getPosition());
}

void TrackGrip::mouseUp (const juce::MouseEvent& event)
{
    if (isEnabled() && onRelease)
        onRelease (event.getPosition());
    // Nothing after the callback: the drop may reorder the Song and lay this grip out again.
}

void TrackGrip::paint (juce::Graphics& g)
{
    // Two columns of three dots, centred.
    constexpr float dot = 2.0f;
    constexpr float step = 4.0f;
    const auto centre = getLocalBounds().toFloat().getCentre();
    g.setColour (colours::dimText.withAlpha (isEnabled() ? 1.0f : 0.35f));
    for (int column = 0; column < 2; ++column)
        for (int row = 0; row < 3; ++row)
            g.fillEllipse (centre.x - step / 2.0f + (float) column * step - dot / 2.0f, centre.y - step + (float) row * step - dot / 2.0f, dot, dot);
}

//==============================================================================

void TrackDrag::begin (const song::Id& trackId, std::size_t from, juce::Rectangle<int> row, juce::Image ghost, int pointerY, const std::vector<juce::Range<int>>& rows)
{
    active = Active { .trackId = trackId, .from = from, .row = row, .ghost = std::move (ghost), .grabOffset = pointerY - row.getY() };
    moveTo (pointerY, rows);
}

void TrackDrag::moveTo (int pointerY, const std::vector<juce::Range<int>>& rows)
{
    if (! active.has_value())
        return;
    active->pointerY = pointerY;
    active->dropIndex = trackDropIndex (rows, active->from, pointerY);
    active->dropLineY = trackDropLineY (rows, active->from, active->dropIndex);
}

std::optional<TrackDrag::Drop> TrackDrag::end (int pointerY, const std::vector<juce::Range<int>>& rows)
{
    if (! active.has_value())
        return std::nullopt;
    moveTo (pointerY, rows);
    const Drop drop { active->trackId, active->from, active->dropIndex };
    active.reset();
    return drop;
}

bool TrackDrag::cancel()
{
    const auto wasActive = active.has_value();
    active.reset();
    return wasActive;
}

std::optional<std::size_t> TrackDrag::dropIndex() const
{
    if (! active.has_value())
        return std::nullopt;
    return active->dropIndex;
}

juce::Rectangle<int> TrackDrag::ghostBounds() const
{
    if (! active.has_value())
        return {};
    return active->row.withY (active->pointerY - active->grabOffset);
}

std::optional<int> TrackDrag::dropLineY() const
{
    if (! active.has_value())
        return std::nullopt;
    return active->dropLineY;
}

void TrackDrag::paint (juce::Graphics& g, int lineLeft, int lineRight) const
{
    if (! active.has_value())
        return;
    const auto ghost = ghostBounds();
    g.setOpacity (ghostOpacity);
    g.drawImage (active->ghost, ghost.toFloat());
    g.setOpacity (1.0f);
    g.setColour (colours::accent);
    g.fillRect (lineLeft, active->dropLineY - dropLineThickness / 2, lineRight - lineLeft, dropLineThickness);
}
} // namespace overdub::ui
