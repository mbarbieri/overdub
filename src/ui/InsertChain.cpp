#include "ui/InsertChain.h"

#include "ui/Colours.h"
#include "ui/Layout.h"

#include <algorithm>

namespace overdub::ui
{
namespace
{
// A press that moves less than this is a click (opens the editor), not a drag.
constexpr int dragThreshold = 4;

const juce::Colour chipColour { 0xff33405a };

plugin::InsertStatus statusOf (const song::InsertSlot& slot, const InsertStatusLookup& lookup)
{
    const auto status = lookup ? lookup (slot.id) : std::optional { plugin::InsertStatus::ready };
    return status.value_or (plugin::InsertStatus::loading);
}
} // namespace

std::string insertChipText (const song::InsertSlot& slot, const InsertStatusLookup& lookup)
{
    switch (statusOf (slot, lookup))
    {
        case plugin::InsertStatus::ready: return slot.name;
        case plugin::InsertStatus::loading: return slot.name + " (loading)";
        case plugin::InsertStatus::failed: return slot.name + " (missing)";
        case plugin::InsertStatus::crashed: return slot.name + " (crashed)";
    }
    return slot.name;
}

std::vector<InsertChipState> insertChipsFor (const std::vector<song::InsertSlot>& chain, const InsertStatusLookup& lookup)
{
    std::vector<InsertChipState> chips;
    for (const auto& slot : chain)
    {
        chips.push_back (InsertChipState {
            .slotId = slot.id,
            .text = insertChipText (slot, lookup),
            .bypassed = slot.bypassed,
            .ready = statusOf (slot, lookup) == plugin::InsertStatus::ready,
        });
    }
    return chips;
}

int activeInsertCount (const std::vector<song::InsertSlot>& chain, const InsertStatusLookup& lookup)
{
    return (int) std::ranges::count_if (chain, [&lookup] (const song::InsertSlot& slot) { return ! slot.bypassed && statusOf (slot, lookup) == plugin::InsertStatus::ready; });
}

//==============================================================================

InsertChip::InsertChip()
{
    // ⏻ and ×, as in the prototype.
    bypassButton.setButtonText (juce::String (juce::CharPointer_UTF8 ("\xe2\x8f\xbb")));
    removeButton.setButtonText (juce::String (juce::CharPointer_UTF8 ("\xc3\x97")));
    bypassButton.setTooltip ("Bypass");
    removeButton.setTooltip ("Remove");
    bypassButton.setColour (juce::TextButton::buttonOnColourId, colours::mute);
    for (auto* button : { &bypassButton, &removeButton })
    {
        button->setWantsKeyboardFocus (false);
        button->setConnectedEdges (juce::Button::ConnectedOnLeft | juce::Button::ConnectedOnRight);
        addAndMakeVisible (*button);
    }
    bypassButton.onClick = [this] {
        if (onBypass)
            onBypass (! current.bypassed);
    };
    removeButton.onClick = [this] {
        if (onRemove)
            onRemove();
    };
}

void InsertChip::show (const InsertChipState& state, bool isEditable)
{
    if (state == current && isEditable == editable)
        return;
    current = state;
    editable = isEditable;
    bypassButton.setToggleState (state.bypassed, juce::dontSendNotification);
    bypassButton.setEnabled (editable);
    removeButton.setEnabled (editable);
    repaint();
}

void InsertChip::mouseDown (const juce::MouseEvent&)
{
    dragging = false;
}

void InsertChip::mouseDrag (const juce::MouseEvent& event)
{
    if (editable && event.getDistanceFromDragStart() >= dragThreshold)
        dragging = true;
}

void InsertChip::mouseUp (const juce::MouseEvent& event)
{
    // A drag is told apart from a click by distance, also when no mouseDrag was delivered.
    const auto moved = event.getPosition() - event.getMouseDownPosition();
    const auto wasDrag = dragging || std::max (std::abs (moved.x), std::abs (moved.y)) >= dragThreshold;
    dragging = false;
    if (wasDrag)
    {
        if (editable && onDragEnd)
            onDragEnd (event.getPosition());
        return;
    }
    if (onOpen)
        onOpen();
    // Nothing after a callback: it may edit the Song and refresh this chain.
}

void InsertChip::resized()
{
    const auto layout = insertChipLayout (getLocalBounds());
    bypassButton.setBounds (layout.bypass);
    removeButton.setBounds (layout.remove);
}

void InsertChip::paint (juce::Graphics& g)
{
    const auto dimmed = current.bypassed || ! current.ready;
    g.setColour (chipColour.withAlpha (dimmed ? 0.45f : 1.0f));
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 4.0f);
    const auto colour = current.ready ? colours::text : colours::dimText;
    g.setColour (current.bypassed ? colour.withAlpha (0.5f) : colour);
    g.setFont (juce::FontOptions (11.0f));
    g.drawText (text(), insertChipLayout (getLocalBounds()).name.reduced (4, 0), juce::Justification::centredLeft, true);
}

//==============================================================================

InsertChainView::InsertChainView (bool isVertical) : vertical (isVertical)
{
    addButton.setWantsKeyboardFocus (false);
    addButton.onClick = [this] {
        if (onAddInsert)
            onAddInsert (trackId);
    };
    addAndMakeVisible (addButton);
}

void InsertChainView::show (const song::Id& chainTrackId, const std::vector<InsertChipState>& states, bool isEditable)
{
    trackId = chainTrackId;
    editable = isEditable;
    addButton.setEnabled (editable);

    // Chips are reused, never deleted while the chain is shown: a gesture's own chip (and the
    // callback it is running) survives the edit and refresh that the gesture triggers.
    while (chips.size() < states.size())
    {
        auto chip = std::make_unique<InsertChip>();
        auto* raw = chip.get();
        const auto index = chips.size();
        chip->onOpen = [this, raw] { emit (SongIntent { intent::OpenInsertEditor { raw->slotId() } }); };
        chip->onBypass = [this, raw] (bool bypassed) { emit (SongIntent { intent::Edit { intent::SetInsertBypassed { raw->slotId(), bypassed } } }); };
        chip->onRemove = [this, raw] { emit (SongIntent { intent::Edit { intent::RemoveInsert { raw->slotId() } } }); };
        chip->onDragEnd = [this, raw, index] (juce::Point<int> end) { dropped (index, end + raw->getPosition()); };
        addChildComponent (*chip);
        chips.push_back (std::move (chip));
    }

    for (std::size_t i = 0; i < states.size(); ++i)
        chips[i]->show (states[i], editable);
    if (states.size() != shown)
    {
        shown = states.size();
        resized();
    }
}

void InsertChainView::dropped (std::size_t index, juce::Point<int> pointInChain)
{
    std::vector<juce::Rectangle<int>> bounds;
    for (std::size_t i = 0; i < shown; ++i)
        bounds.push_back (chips[i]->isVisible() ? chips[i]->getBounds() : juce::Rectangle<int>());
    const auto toIndex = insertDropIndex (bounds, index, pointInChain, vertical);
    if (toIndex != index)
        emit (SongIntent { intent::Edit { intent::MoveInsert { chips[index]->slotId(), toIndex } } });
}

void InsertChainView::emit (Intent intent) const
{
    if (onIntent)
        onIntent (intent);
}

void InsertChainView::resized()
{
    const auto layout = insertChainLayout (getLocalBounds(), (int) shown, vertical);
    addButton.setBounds (layout.addButton);
    for (std::size_t i = 0; i < chips.size(); ++i)
    {
        const auto bounds = i < shown ? layout.chips[i] : juce::Rectangle<int>();
        chips[i]->setBounds (bounds);
        chips[i]->setVisible (! bounds.isEmpty());
    }
}
} // namespace overdub::ui
