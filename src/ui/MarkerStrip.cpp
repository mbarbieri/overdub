#include "ui/MarkerStrip.h"

#include "ui/Colours.h"
#include "ui/TransportText.h"

#include <algorithm>
#include <cmath>
#include <ranges>

namespace overdub::ui
{
namespace
{
// A flag moves only once the mouse has gone this far, so a click never moves a Marker.
constexpr int dragThreshold = 3;
constexpr int markerRowHeight = 22;
constexpr int markerHeadingHeight = 20;
const juce::Colour markerColour { 0xff64d2ff };

juce::FontOptions flagFont()
{
    return juce::FontOptions (10.0f, juce::Font::bold);
}

int flagWidth (const juce::String& name)
{
    return juce::GlyphArrangement::getStringWidthInt (juce::Font (flagFont()), name) + markerFlagPadding;
}

// The screen distance the mouse moved since its press: the flag moves under it during a drag.
int dragDeltaX (const juce::MouseEvent& event)
{
    return event.getScreenX() - event.getMouseDownScreenX();
}

std::vector<song::Id> idsOf (const std::vector<song::Marker>& markers)
{
    std::vector<song::Id> ids;
    for (const auto& marker : markers)
        ids.push_back (marker.id);
    return ids;
}
} // namespace

StripScale stripScaleFor (const TimelineView& view)
{
    return StripScale { (double) view.firstSample, (double) std::max<SampleCount> (1, view.samplesPerPixel) };
}

StripScale stripScaleFitting (song::SampleCount length, int width)
{
    if (length <= 0 || width <= 0)
        return StripScale {};
    return StripScale { 0.0, (double) length / (double) width };
}

int stripX (const StripScale& scale, song::SampleCount sample)
{
    return (int) std::floor (((double) sample - scale.firstSample) / scale.samplesPerPixel);
}

song::SampleCount draggedMarkerPosition (const StripScale& scale, song::SampleCount position, int deltaX)
{
    const auto moved = (double) position + (double) deltaX * scale.samplesPerPixel;
    return std::max<song::SampleCount> (0, (song::SampleCount) std::llround (moved));
}

//==============================================================================
MarkerFlag::MarkerFlag (song::Id markerId)
    : id (std::move (markerId))
{
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

void MarkerFlag::show (const song::Marker& marker, bool isSelected)
{
    const auto name = juce::String::fromUTF8 (marker.name.c_str());
    if (name == shownName && isSelected == selected)
        return;
    shownName = name;
    selected = isSelected;
    repaint();
}

void MarkerFlag::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds();
    if (selected)
    {
        g.setColour (markerColour.withAlpha (0.3f));
        g.fillRect (bounds);
    }
    g.setColour (markerColour);
    g.fillRect (0, 0, 2, bounds.getHeight());
    juce::Path head;
    head.addTriangle (0.0f, 0.0f, 7.0f, 0.0f, 0.0f, 7.0f);
    g.fillPath (head);
    g.setColour (selected ? colours::text : markerColour);
    g.setFont (flagFont());
    g.drawText (shownName, bounds.withTrimmedLeft (markerFlagPadding - 4), juce::Justification::centredLeft, true);
}

void MarkerFlag::mouseDown (const juce::MouseEvent&)
{
    dragging = false;
}

void MarkerFlag::mouseDrag (const juce::MouseEvent& event)
{
    const auto deltaX = dragDeltaX (event);
    if (! dragging && std::abs (deltaX) < dragThreshold)
        return;
    dragging = true;
    if (onDrag)
        onDrag (deltaX);
}

void MarkerFlag::mouseUp (const juce::MouseEvent& event)
{
    if (dragging)
    {
        dragging = false;
        if (onDragEnd)
            onDragEnd (dragDeltaX (event));
        return;
    }
    if (onClick)
        onClick();
}

void MarkerFlag::mouseDoubleClick (const juce::MouseEvent&)
{
    if (onDoubleClick)
        onDoubleClick();
}

//==============================================================================
MarkerStrip::MarkerStrip()
{
    setMouseCursor (juce::MouseCursor::NormalCursor);
    nameField.setFont (flagFont());
    nameField.setColour (juce::Label::textColourId, colours::text);
    nameField.setColour (juce::Label::backgroundColourId, colours::background);
    nameField.setColour (juce::Label::outlineColourId, markerColour);
    nameField.setBorderSize ({ 1, 4, 1, 4 });
    nameField.onTextChange = [this] {
        if (! renaming.has_value())
            return;
        const auto* marker = markerWithId (*renaming);
        const auto typed = nameField.getText().toStdString();
        if (marker != nullptr && typed != marker->name)
            emit (SongIntent { intent::Edit { intent::RenameMarker { *renaming, typed } } });
    };
    // Label::setText hides the editor before it reports the new text, so `renaming` outlives
    // the editor: it names the Marker the last double-click opened the field for.
    nameField.onEditorHide = [this] { nameField.setVisible (false); };
    addChildComponent (nameField);
}

void MarkerStrip::show (const std::vector<song::Marker>& newMarkers, StripScale newScale, bool isEditable, std::optional<song::Id> newSelected)
{
    if (newMarkers == markers && newScale == scale && isEditable == editable && newSelected == selected)
        return;
    const auto idsChanged = idsOf (newMarkers) != idsOf (markers);
    markers = newMarkers;
    scale = newScale;
    editable = isEditable;
    selected = std::move (newSelected);
    if (idsChanged)
        flags.clear();
    showFlags();
    if (! editable && nameField.isBeingEdited())
        nameField.hideEditor (true);
    repaint();
}

void MarkerStrip::showFlags()
{
    if (flags.empty())
        for (const auto& marker : markers)
        {
            auto& flag = *flags.emplace_back (std::make_unique<MarkerFlag> (marker.id));
            const auto markerId = marker.id;
            flag.onClick = [this, markerId] { emit (SongIntent { intent::SelectMarker { markerId } }); };
            flag.onDrag = [this, &flag] (int deltaX) {
                if (const auto* found = markerWithId (flag.markerId()); found != nullptr && editable)
                    placeFlag (flag, draggedMarkerPosition (scale, found->position, deltaX));
            };
            flag.onDragEnd = [this, markerId] (int deltaX) {
                const auto* found = markerWithId (markerId);
                if (found == nullptr || ! editable)
                    return;
                const auto position = draggedMarkerPosition (scale, found->position, deltaX);
                if (position != found->position)
                    emit (SongIntent { intent::Edit { intent::MoveMarker { markerId, position } } });
                // Every flag back where its Marker is now: moved, refused or not moved at all.
                showFlags();
            };
            flag.onDoubleClick = [this, markerId] { startRenaming (markerId); };
            addAndMakeVisible (flag);
        }
    for (std::size_t i = 0; i < flags.size() && i < markers.size(); ++i)
    {
        flags[i]->show (markers[i], selected == markers[i].id);
        placeFlag (*flags[i], markers[i].position);
    }
    nameField.toFront (false);
}

void MarkerStrip::placeFlag (MarkerFlag& flag, song::SampleCount position)
{
    flag.setBounds (stripX (scale, position), 0, flagWidth (flag.name()), getHeight());
}

void MarkerStrip::startRenaming (const song::Id& markerId)
{
    const auto* marker = markerWithId (markerId);
    if (! editable || marker == nullptr)
        return;
    const auto flag = std::ranges::find (flags, markerId, [] (const auto& f) { return f->markerId(); });
    if (flag == flags.end())
        return;
    renaming = markerId;
    nameField.setText (juce::String::fromUTF8 (marker->name.c_str()), juce::dontSendNotification);
    nameField.setBounds ((*flag)->getBounds().withWidth (std::max (120, (*flag)->getWidth() + 40)));
    nameField.setVisible (true);
    nameField.toFront (false);
    nameField.showEditor();
}

const song::Marker* MarkerStrip::markerWithId (const song::Id& markerId) const
{
    const auto found = std::ranges::find (markers, markerId, &song::Marker::id);
    return found == markers.end() ? nullptr : &*found;
}

void MarkerStrip::paint (juce::Graphics& g)
{
    g.setColour (colours::panel.darker (0.3f));
    g.fillRect (getLocalBounds());
    g.setColour (colours::line);
    g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());
}

void MarkerStrip::resized()
{
    showFlags();
}

void MarkerStrip::mouseDown (const juce::MouseEvent&)
{
    if (editable)
        emit (SongIntent { intent::SelectMarker { std::nullopt } });
}

void MarkerStrip::emit (const Intent& intent) const
{
    if (onIntent)
        onIntent (intent);
}

//==============================================================================
MarkerRow::MarkerRow (song::Id markerId)
    : id (std::move (markerId))
{
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

void MarkerRow::show (const song::Marker& marker, int sampleRate, bool isSelected)
{
    const auto newName = juce::String::fromUTF8 (marker.name.c_str());
    const auto newTime = juce::String::fromUTF8 (clockText (marker.position, sampleRate).c_str());
    if (newName == name && newTime == time && isSelected == selected)
        return;
    name = newName;
    time = newTime;
    selected = isSelected;
    repaint();
}

void MarkerRow::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().reduced (2, 1);
    if (selected)
    {
        g.setColour (markerColour.withAlpha (0.25f));
        g.fillRoundedRectangle (bounds.toFloat(), 3.0f);
    }
    g.setColour (markerColour);
    g.fillRect (bounds.removeFromLeft (3).reduced (0, 4));
    bounds.removeFromLeft (6);
    g.setFont (juce::FontOptions (11.0f));
    g.setColour (colours::dimText);
    g.drawText (time, bounds.removeFromRight (64), juce::Justification::centredRight, false);
    g.setColour (colours::text);
    g.drawText (name, bounds, juce::Justification::centredLeft, true);
}

void MarkerRow::mouseUp (const juce::MouseEvent&)
{
    if (onClick)
        onClick();
}

//==============================================================================
MarkerList::MarkerList()
{
    heading.setColour (juce::Label::textColourId, colours::dimText);
    heading.setFont (juce::FontOptions (9.5f, juce::Font::bold));
    heading.setBorderSize ({});
    heading.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (heading);
    hint.setText ("No Markers. Press K to add one at the playhead.", juce::dontSendNotification);
    hint.setColour (juce::Label::textColourId, colours::dimText);
    hint.setFont (juce::FontOptions (11.0f));
    hint.setJustificationType (juce::Justification::topLeft);
    hint.setBorderSize ({});
    hint.setInterceptsMouseClicks (false, false);
    addChildComponent (hint);
}

void MarkerList::show (const std::vector<song::Marker>& markers, int sampleRate, std::optional<song::Id> selected)
{
    const auto sameIds = std::ranges::equal (markers, rows, {}, &song::Marker::id, [] (const auto& row) { return row->markerId(); });
    if (! sameIds)
    {
        rows.clear();
        for (const auto& marker : markers)
        {
            auto& row = *rows.emplace_back (std::make_unique<MarkerRow> (marker.id));
            row.onClick = [this, markerId = marker.id] {
                if (onIntent)
                    onIntent (SongIntent { intent::SelectMarker { markerId } });
            };
            addAndMakeVisible (row);
        }
        layOutRows();
    }
    for (std::size_t i = 0; i < rows.size(); ++i)
        rows[i]->show (markers[i], sampleRate, selected == markers[i].id);
    hint.setVisible (markers.empty());
}

void MarkerList::resized()
{
    layOutRows();
}

void MarkerList::layOutRows()
{
    auto area = getLocalBounds().reduced (8, 6);
    heading.setBounds (area.removeFromTop (markerHeadingHeight));
    hint.setBounds (area.withHeight (60));
    for (auto& row : rows)
        row->setBounds (area.removeFromTop (markerRowHeight));
}

void MarkerList::paint (juce::Graphics& g)
{
    g.setColour (colours::panel);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 6.0f);
}
} // namespace overdub::ui
