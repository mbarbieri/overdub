#include "ui/EditPage.h"

#include "song/ClipEdits.h"
#include "ui/Colours.h"
#include "ui/Layout.h"

#include <cctype>
#include <cmath>
#include <cstdlib>
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

namespace
{
std::string trimmed (const std::string& text)
{
    const auto first = text.find_first_not_of (" \t");
    if (first == std::string::npos)
        return {};
    const auto last = text.find_last_not_of (" \t");
    return text.substr (first, last - first + 1);
}

// The text without a trailing unit (any case) and the spaces around both; unchanged without it.
std::string withoutUnit (const std::string& text, const std::string& unit)
{
    auto value = trimmed (text);
    if (value.size() < unit.size())
        return value;
    const auto tail = value.substr (value.size() - unit.size());
    for (std::size_t i = 0; i < unit.size(); ++i)
        if (std::tolower ((unsigned char) tail[i]) != unit[i])
            return value;
    return trimmed (value.substr (0, value.size() - unit.size()));
}

// The whole text as a finite number, or none.
std::optional<double> parseNumber (const std::string& text)
{
    if (text.empty())
        return std::nullopt;
    char* end = nullptr;
    const auto value = std::strtod (text.c_str(), &end);
    if (end != text.c_str() + text.size() || ! std::isfinite (value))
        return std::nullopt;
    return value;
}
} // namespace

std::optional<song::SampleCount> parseSeconds (const std::string& text, int sampleRate)
{
    const auto seconds = parseNumber (withoutUnit (text, "s"));
    if (! seconds.has_value() || *seconds < 0.0 || sampleRate <= 0)
        return std::nullopt;
    return (song::SampleCount) std::llround (*seconds * sampleRate);
}

std::optional<double> parseDecibels (const std::string& text)
{
    return parseNumber (withoutUnit (text, "db"));
}

ClipToolbarState clipToolbarStateFor (const song::Song& song, const std::optional<song::Id>& selectedClip, song::SampleCount playhead, bool editable)
{
    ClipToolbarState state;
    const auto* clip = selectedClip.has_value() ? song::findClip (song, *selectedClip) : nullptr;
    if (clip == nullptr)
    {
        state.details = "Select a Clip. Drag it to move, its edges to trim, its top corners to fade, its dB tag for gain. S splits at the playhead.";
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
    state.canEditFadesAndGain = editable;
    state.fadeInText = secondsText (clip->fadeInLength, song.sampleRate);
    state.fadeOutText = secondsText (clip->fadeOutLength, song.sampleRate);
    state.gainText = decibelsText (clip->gainDb);
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

    for (auto* caption : { &fadeInCaption, &fadeOutCaption, &gainCaption })
    {
        caption->setColour (juce::Label::textColourId, colours::dimText);
        caption->setFont (juce::FontOptions (11.5f));
        caption->setJustificationType (juce::Justification::centredRight);
        caption->setInterceptsMouseClicks (false, false);
        addAndMakeVisible (caption);
    }

    // `callback` is one of this toolbar's members, so the reference outlives the field.
    const auto setUpField = [] (ClipToolbar& toolbar, juce::Label& field, std::function<void (const std::string&)>& callback) {
        field.setEditable (true, false, false);
        field.setFont (juce::FontOptions (12.0f));
        field.setJustificationType (juce::Justification::centred);
        field.setColour (juce::Label::textColourId, colours::text);
        field.setColour (juce::Label::backgroundColourId, colours::background);
        field.setColour (juce::Label::outlineColourId, colours::line);
        field.onTextChange = [&field, &callback] {
            if (callback)
                callback (field.getText().toStdString());
        };
        toolbar.addAndMakeVisible (field);
    };
    setUpField (*this, fadeInField, onFadeInEntered);
    setUpField (*this, fadeOutField, onFadeOutEntered);
    setUpField (*this, gainField, onGainEntered);
}

void ClipToolbar::show (const ClipToolbarState& state, bool mixerVisible)
{
    details.setText (juce::String::fromUTF8 (state.details.c_str()), juce::dontSendNotification);
    details.setColour (juce::Label::textColourId, state.hasClip ? colours::text : colours::dimText);
    splitButton.setEnabled (state.canSplit);
    deleteButton.setEnabled (state.canDelete);
    for (auto [field, text] : { std::pair { &fadeInField, &state.fadeInText }, std::pair { &fadeOutField, &state.fadeOutText }, std::pair { &gainField, &state.gainText } })
    {
        field->setText (juce::String::fromUTF8 (text->c_str()), juce::dontSendNotification);
        field->setEnabled (state.canEditFadesAndGain);
    }
    mixerButton.setToggleState (mixerVisible, juce::dontSendNotification);
}

void ClipToolbar::resized()
{
    const auto layout = clipToolbarLayout (getLocalBounds());
    details.setBounds (layout.details);
    fadeInCaption.setBounds (layout.fadeInCaption);
    fadeInField.setBounds (layout.fadeInField);
    fadeOutCaption.setBounds (layout.fadeOutCaption);
    fadeOutField.setBounds (layout.fadeOutField);
    gainCaption.setBounds (layout.gainCaption);
    gainField.setBounds (layout.gainField);
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
    toolbar.onFadeInEntered = [this] (const std::string& text) {
        const auto samples = parseSeconds (text, current.song.sampleRate);
        editSelectedClip ([&] (const song::Song& song, const song::Id& clipId) -> core::Expected<song::Song> {
            if (! samples.has_value())
                return song;
            return song::setClipFadeIn (song, clipId, *samples);
        }, song::fadeInLabel);
    };
    toolbar.onFadeOutEntered = [this] (const std::string& text) {
        const auto samples = parseSeconds (text, current.song.sampleRate);
        editSelectedClip ([&] (const song::Song& song, const song::Id& clipId) -> core::Expected<song::Song> {
            if (! samples.has_value())
                return song;
            return song::setClipFadeOut (song, clipId, *samples);
        }, song::fadeOutLabel);
    };
    toolbar.onGainEntered = [this] (const std::string& text) {
        const auto gainDb = parseDecibels (text);
        editSelectedClip ([&] (const song::Song& song, const song::Id& clipId) -> core::Expected<song::Song> {
            if (! gainDb.has_value())
                return song;
            return song::setClipGain (song, clipId, *gainDb);
        }, song::clipGainLabel);
    };

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

void EditPage::editSelectedClip (const std::function<core::Expected<song::Song> (const song::Song&, const song::Id&)>& change, const char* label)
{
    const auto& clipId = timeline.selectedClip();
    if (clipId.has_value() && current.editable)
    {
        auto changed = change (current.song, *clipId);
        if (changed.has_value() && *changed != current.song && onEdit)
            onEdit (std::move (*changed), label);
    }
    showToolbar();
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
