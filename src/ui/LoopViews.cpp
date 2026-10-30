#include "ui/LoopViews.h"

#include "song/PracticeSpeed.h"
#include "ui/Colours.h"
#include "ui/TransportText.h"

#include <algorithm>
#include <cmath>

namespace overdub::ui
{
namespace
{
// An edge moves only once the mouse has gone this far, so a click never resizes a Loop.
constexpr int dragThreshold = 3;
constexpr int edgeGrabWidth = 5;
constexpr int loopCardHeight = 34;
constexpr int loopHeadingHeight = 20;
const juce::Colour loopColour { 0xffffb340 };

int dragDeltaX (const juce::MouseEvent& event)
{
    return event.getScreenX() - event.getMouseDownScreenX();
}

juce::String fromUtf8 (const std::string& text)
{
    return juce::String::fromUTF8 (text.c_str());
}
} // namespace

std::string loopRangeText (const song::Loop& loop, int sampleRate)
{
    return clockText (loop.start, sampleRate) + " \xe2\x80\x93 " + clockText (loop.end, sampleRate);
}

std::string loopLengthText (const song::Loop& loop, int sampleRate)
{
    return clockText (loop.length(), sampleRate);
}

std::string loopSpeedText (const song::Loop& loop)
{
    return std::to_string (loop.settings.practiceSpeedPercent) + "%";
}

std::string loopKeyText (std::size_t index)
{
    return index < 9 ? "\xe2\x8c\xa5" + std::to_string (index + 1) : std::string {};
}

//==============================================================================
LoopBar::LoopBar (song::Id loopId)
    : id (std::move (loopId))
{
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

void LoopBar::show (const song::Loop& loop, bool isActive, bool isSelected, bool isEditable)
{
    const auto name = fromUtf8 (loop.name);
    if (name == shownName && isActive == active && isSelected == selected && isEditable == editable)
        return;
    shownName = name;
    active = isActive;
    selected = isSelected;
    editable = isEditable;
    repaint();
}

std::optional<LoopEdge> LoopBar::edgeAt (int x) const
{
    if (! editable)
        return std::nullopt;
    const auto grab = std::min (edgeGrabWidth, getWidth() / 3);
    if (x < grab)
        return LoopEdge::start;
    if (x >= getWidth() - grab)
        return LoopEdge::end;
    return std::nullopt;
}

void LoopBar::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds();
    g.setColour (loopColour.withAlpha (active ? 0.45f : selected ? 0.35f
                                                                 : 0.18f));
    g.fillRect (bounds);
    g.setColour (loopColour.withAlpha (active || selected ? 1.0f : 0.6f));
    g.fillRect (0, 0, 2, bounds.getHeight());
    g.fillRect (bounds.getWidth() - 2, 0, 2, bounds.getHeight());
    g.setColour (active ? colours::text : loopColour);
    g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
    g.drawText (shownName, bounds.reduced (5, 0), juce::Justification::centredLeft, true);
}

void LoopBar::mouseMove (const juce::MouseEvent& event)
{
    setMouseCursor (edgeAt (event.x).has_value() ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::PointingHandCursor);
}

void LoopBar::mouseDown (const juce::MouseEvent& event)
{
    grabbed = edgeAt (event.x);
    dragging = false;
}

void LoopBar::mouseDrag (const juce::MouseEvent& event)
{
    const auto deltaX = dragDeltaX (event);
    if (! grabbed.has_value() || (! dragging && std::abs (deltaX) < dragThreshold))
        return;
    dragging = true;
    if (onEdgeDrag)
        onEdgeDrag (*grabbed, deltaX);
}

void LoopBar::mouseUp (const juce::MouseEvent& event)
{
    if (dragging && grabbed.has_value())
    {
        dragging = false;
        const auto edge = *grabbed;
        grabbed.reset();
        if (onEdgeDragEnd)
            onEdgeDragEnd (edge, dragDeltaX (event));
        return;
    }
    grabbed.reset();
    // A body drag is not a gesture; only a click selects.
    if (std::abs (dragDeltaX (event)) >= dragThreshold)
        return;
    if (onClick)
        onClick();
}

void LoopBar::mouseDoubleClick (const juce::MouseEvent&)
{
    if (onDoubleClick)
        onDoubleClick();
}

//==============================================================================
LoopCard::LoopCard (song::Id loopId)
    : id (std::move (loopId))
{
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

void LoopCard::show (const song::Loop& loop, int sampleRate, std::size_t index, bool isActive)
{
    const auto newName = fromUtf8 (loop.name);
    const auto newRange = fromUtf8 (loopRangeText (loop, sampleRate));
    const auto newLength = fromUtf8 (loopLengthText (loop, sampleRate));
    const auto newSpeed = fromUtf8 (loopSpeedText (loop));
    const auto newKey = fromUtf8 (loopKeyText (index));
    const bool newAtNormalSpeed = loop.settings.practiceSpeedPercent == song::normalPracticeSpeed;
    if (newName == name && newRange == range && newLength == length && newSpeed == speed && newKey == key && isActive == active)
        return;
    atNormalSpeed = newAtNormalSpeed;
    name = newName;
    range = newRange;
    length = newLength;
    speed = newSpeed;
    key = newKey;
    active = isActive;
    repaint();
}

void LoopCard::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().reduced (2, 2);
    g.setColour (active ? loopColour.withAlpha (0.25f) : colours::panel.brighter (0.06f));
    g.fillRoundedRectangle (bounds.toFloat(), 4.0f);
    if (active)
    {
        g.setColour (loopColour);
        g.drawRoundedRectangle (bounds.toFloat(), 4.0f, 1.0f);
    }
    bounds.reduce (8, 3);
    auto top = bounds.removeFromTop (bounds.getHeight() / 2);
    g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
    g.setColour (colours::dimText);
    g.drawText (key, top.removeFromRight (24), juce::Justification::centredRight, false);
    // A Loop drilled at the Song's own speed shows it dimmed.
    g.setColour (atNormalSpeed ? colours::dimText : loopColour);
    g.drawText (speed, top.removeFromRight (36), juce::Justification::centredRight, false);
    g.setFont (juce::FontOptions (11.5f, juce::Font::bold));
    g.setColour (active ? colours::text : loopColour);
    g.drawText (name, top, juce::Justification::centredLeft, true);
    g.setFont (juce::FontOptions (10.5f));
    g.setColour (colours::dimText);
    g.drawText (length, bounds.removeFromRight (48), juce::Justification::centredRight, false);
    g.drawText (range, bounds, juce::Justification::centredLeft, true);
}

void LoopCard::mouseUp (const juce::MouseEvent&)
{
    if (onClick)
        onClick();
}

//==============================================================================
LoopList::LoopList()
{
    heading.setColour (juce::Label::textColourId, colours::dimText);
    heading.setFont (juce::FontOptions (9.5f, juce::Font::bold));
    heading.setBorderSize ({});
    heading.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (heading);
    hint.setText ("No Loops. Drag on the strip, or press [ and ] while you listen.", juce::dontSendNotification);
    hint.setColour (juce::Label::textColourId, colours::dimText);
    hint.setFont (juce::FontOptions (11.0f));
    hint.setJustificationType (juce::Justification::topLeft);
    hint.setBorderSize ({});
    hint.setInterceptsMouseClicks (false, false);
    addChildComponent (hint);
}

void LoopList::show (const std::vector<song::Loop>& loops, int sampleRate, std::optional<song::Id> active)
{
    const auto sameIds = std::ranges::equal (loops, cards, {}, &song::Loop::id, [] (const auto& card) { return card->loopId(); });
    if (! sameIds)
    {
        cards.clear();
        for (const auto& loop : loops)
        {
            auto& card = *cards.emplace_back (std::make_unique<LoopCard> (loop.id));
            card.onClick = [this, loopId = loop.id] {
                if (onIntent)
                    onIntent (SongIntent { intent::SelectLoop { loopId } });
            };
            addAndMakeVisible (card);
        }
        layOutCards();
    }
    for (std::size_t i = 0; i < cards.size(); ++i)
        cards[i]->show (loops[i], sampleRate, i, active == loops[i].id);
    hint.setVisible (loops.empty());
}

void LoopList::resized()
{
    layOutCards();
}

void LoopList::layOutCards()
{
    auto area = getLocalBounds().reduced (8, 6);
    heading.setBounds (area.removeFromTop (loopHeadingHeight));
    hint.setBounds (area.withHeight (60));
    for (auto& card : cards)
        card->setBounds (area.removeFromTop (loopCardHeight));
}

void LoopList::paint (juce::Graphics& g)
{
    g.setColour (colours::panel);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 6.0f);
}
} // namespace overdub::ui
