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
        if (actions != nullptr && actions->add)
            actions->add (trackId);
    };
    addAndMakeVisible (addButton);
}

void InsertChainView::show (const song::Id& chainTrackId, const std::vector<InsertChipState>& states, bool isEditable)
{
    trackId = chainTrackId;
    editable = isEditable;
    addButton.setEnabled (editable);

    const auto sameSlots = std::ranges::equal (chips, states, {}, &InsertChip::slotId, &InsertChipState::slotId);
    if (! sameSlots)
    {
        chips.clear();
        for (std::size_t i = 0; i < states.size(); ++i)
        {
            auto chip = std::make_unique<InsertChip>();
            auto* raw = chip.get();
            chip->onOpen = [this, raw] {
                if (actions != nullptr && actions->open)
                    actions->open (raw->slotId());
            };
            chip->onBypass = [this, raw] (bool bypassed) {
                if (actions != nullptr && actions->bypass)
                    actions->bypass (raw->slotId(), bypassed);
            };
            chip->onRemove = [this, raw] {
                if (actions != nullptr && actions->remove)
                    actions->remove (raw->slotId());
            };
            chip->onDragEnd = [this, raw, i] (juce::Point<int> end) { dropped (i, end + raw->getPosition()); };
            addAndMakeVisible (*chip);
            chips.push_back (std::move (chip));
        }
        resized();
    }
    for (std::size_t i = 0; i < chips.size(); ++i)
        chips[i]->show (states[i], editable);
}

void InsertChainView::dropped (std::size_t index, juce::Point<int> pointInChain)
{
    std::vector<juce::Rectangle<int>> bounds;
    for (const auto& chip : chips)
        bounds.push_back (chip->isVisible() ? chip->getBounds() : juce::Rectangle<int>());
    const auto toIndex = insertDropIndex (bounds, index, pointInChain, vertical);
    if (toIndex == index || actions == nullptr || ! actions->move)
        return;
    actions->move (chips[index]->slotId(), toIndex);
}

void InsertChainView::resized()
{
    const auto layout = insertChainLayout (getLocalBounds(), (int) chips.size(), vertical);
    addButton.setBounds (layout.addButton);
    for (std::size_t i = 0; i < chips.size(); ++i)
    {
        chips[i]->setBounds (layout.chips[i]);
        chips[i]->setVisible (! layout.chips[i].isEmpty());
    }
}
} // namespace overdub::ui
