#include "ui/EditPage.h"

#include "song/ClipEdits.h"
#include "ui/Colours.h"
#include "ui/Layout.h"

#include <format>

namespace overdub::ui
{
std::string secondsText (song::SampleCount samples, int sampleRate)
{
    if (sampleRate <= 0)
        return "0.000 s";
    const auto sign = samples < 0 ? "-" : "";
    const auto magnitude = samples < 0 ? -samples : samples;
    const auto milliseconds = magnitude * 1000 / sampleRate;
    return std::format ("{}{}.{:03} s", sign, milliseconds / 1000, milliseconds % 1000);
}

ClipToolbarState clipToolbarStateFor (const song::Song& song, const std::optional<song::Id>& selectedClip, song::SampleCount playhead, bool editable)
{
    ClipToolbarState state;
    const auto* clip = selectedClip.has_value() ? song::findClip (song, *selectedClip) : nullptr;
    if (clip == nullptr)
    {
        state.details = "Select a Clip. Drag it to move, drag its edges to trim. S splits at the playhead.";
        return state;
    }

    const auto* track = song::trackOfClip (song, clip->id);
    state.hasClip = true;
    state.details = std::format ("{} on {} \xC2\xB7 start {} \xC2\xB7 length {}",
                                 clipName (song, *clip),
                                 track->name,
                                 secondsText (clip->timelineStart, song.sampleRate),
                                 secondsText (clip->length, song.sampleRate));
    state.canSplit = editable && playhead > clip->timelineStart && playhead < clip->timelineStart + clip->length;
    state.canDelete = editable;
    return state;
}

//==============================================================================

ClipToolbar::ClipToolbar()
{
    details.setColour (juce::Label::textColourId, colours::text);
    details.setFont (juce::FontOptions (12.5f));
    addAndMakeVisible (details);

    mixerButton.setClickingTogglesState (false);
    mixerButton.setColour (juce::TextButton::buttonOnColourId, colours::accent);

    splitButton.onClick = [this] {
        if (onSplit)
            onSplit();
    };
    deleteButton.onClick = [this] {
        if (onDelete)
            onDelete();
    };
    zoomInButton.onClick = [this] {
        if (onZoomIn)
            onZoomIn();
    };
    zoomOutButton.onClick = [this] {
        if (onZoomOut)
            onZoomOut();
    };
    mixerButton.onClick = [this] {
        if (onToggleMixer)
            onToggleMixer();
    };
    for (auto* button : { &splitButton, &deleteButton, &zoomOutButton, &zoomInButton, &mixerButton })
    {
        button->setWantsKeyboardFocus (false);
        addAndMakeVisible (button);
    }
}

void ClipToolbar::show (const ClipToolbarState& state, bool mixerVisible)
{
    details.setText (juce::String::fromUTF8 (state.details.c_str()), juce::dontSendNotification);
    details.setColour (juce::Label::textColourId, state.hasClip ? colours::text : colours::dimText);
    splitButton.setEnabled (state.canSplit);
    deleteButton.setEnabled (state.canDelete);
    mixerButton.setToggleState (mixerVisible, juce::dontSendNotification);
}

void ClipToolbar::resized()
{
    const auto layout = clipToolbarLayout (getLocalBounds());
    details.setBounds (layout.details);
    splitButton.setBounds (layout.split);
    deleteButton.setBounds (layout.deleteClip);
    zoomOutButton.setBounds (layout.zoomOut);
    zoomInButton.setBounds (layout.zoomIn);
    mixerButton.setBounds (layout.mixerToggle);
}

void ClipToolbar::paint (juce::Graphics& g)
{
    g.fillAll (colours::panel);
    g.setColour (colours::line);
    g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());
}

//==============================================================================

MixerDivider::MixerDivider()
{
    setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
}

void MixerDivider::mouseDown (const juce::MouseEvent& event)
{
    grabOffset = event.getPosition().y;
}

void MixerDivider::mouseDrag (const juce::MouseEvent& event)
{
    if (getParentComponent() == nullptr || ! onDragTo)
        return;
    onDragTo (event.getEventRelativeTo (getParentComponent()).getPosition().y - grabOffset);
}

void MixerDivider::paint (juce::Graphics& g)
{
    g.fillAll (colours::line);
}

void MixerRegion::paint (juce::Graphics& g)
{
    g.fillAll (colours::panel.darker (0.2f));
    g.setColour (colours::dimText);
    g.setFont (juce::FontOptions (13.0f));
    g.drawText ("The Mixer arrives in a later version.", getLocalBounds(), juce::Justification::centred);
}

//==============================================================================

EditPage::EditPage()
{
    toolbar.onSplit = [this] {
        if (onSplit)
            onSplit();
    };
    toolbar.onDelete = [this] {
        if (onDelete)
            onDelete();
    };
    toolbar.onZoomIn = [this] { timeline.zoomIn(); };
    toolbar.onZoomOut = [this] { timeline.zoomOut(); };
    toolbar.onToggleMixer = [this] { setMixerVisible (! mixerVisible); };

    timeline.onEdit = [this] (song::Song changed, std::string label) {
        if (onEdit)
            onEdit (std::move (changed), std::move (label));
    };
    timeline.onSeek = [this] (song::SampleCount sample) {
        if (onSeek)
            onSeek (sample);
    };
    timeline.onArm = [this] (const song::Id& trackId, bool armed) {
        if (onArm)
            onArm (trackId, armed);
    };
    timeline.onSelectionChanged = [this] { showToolbar(); };

    divider.onDragTo = [this] (int top) {
        mixerHeightRequested = clampMixerHeight (getHeight() - top - mixerDividerHeight, getHeight());
        resized();
    };

    addAndMakeVisible (toolbar);
    addAndMakeVisible (timeline);
    addAndMakeVisible (divider);
    addAndMakeVisible (mixer);
    showToolbar();
}

void EditPage::show (const TimelineState& state)
{
    current = state;
    timeline.show (current);
    showToolbar();
}

void EditPage::showToolbar()
{
    toolbar.show (clipToolbarStateFor (current.song, timeline.selectedClip(), current.playhead, current.editable), mixerVisible);
}

void EditPage::setMixerVisible (bool visible)
{
    mixerVisible = visible;
    divider.setVisible (visible);
    mixer.setVisible (visible);
    showToolbar();
    resized();
}

void EditPage::resized()
{
    const auto layout = editPageLayout (getLocalBounds(), mixerVisible, mixerHeightRequested);
    toolbar.setBounds (layout.clipToolbar);
    timeline.setBounds (layout.timeline);
    divider.setBounds (layout.mixerDivider);
    mixer.setBounds (layout.mixer);
}

void EditPage::paint (juce::Graphics& g)
{
    g.fillAll (colours::background);
}
} // namespace overdub::ui
