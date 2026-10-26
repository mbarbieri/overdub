#include "ui/EditPage.h"

#include "song/TakeEdits.h"
#include "song/VideoTakeEdits.h"
#include "ui/Colours.h"
#include "ui/Layout.h"
#include "ui/SongEdits.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <format>

namespace overdub::ui
{

namespace
{
constexpr const char* noSelectionHint = "Select a Clip. Drag it to move, its edges to trim, its top corners to fade, its dB tag for gain. S splits at the playhead.";

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
        state.details = noSelectionHint;
        return state;
    }

    const auto* track = song::trackOfClip (song, clip->id);
    state.hasSelection = true;
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

ClipToolbarState videoTakeToolbarStateFor (const song::Song& song, const std::vector<video::VideoTakeSpan>& videoTakes, const song::Id& videoTakeId, bool editable)
{
    ClipToolbarState state;
    const auto span = std::ranges::find (videoTakes, videoTakeId, &video::VideoTakeSpan::videoTakeId);
    if (span == videoTakes.end() || song::findVideoTake (song, videoTakeId) == nullptr)
    {
        state.details = noSelectionHint;
        return state;
    }

    const auto length = span->readable ? "length " + secondsText (span->end - span->start, song.sampleRate) : std::string ("movie missing");
    state.hasSelection = true;
    state.details = std::format ("Video Take \xC2\xB7 {} \xC2\xB7 start {} \xC2\xB7 {}", span->title, secondsText (span->start, song.sampleRate), length);
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
    details.setColour (juce::Label::textColourId, state.hasSelection ? colours::text : colours::dimText);
    splitButton.setEnabled (state.canSplit);
    deleteButton.setEnabled (state.canDelete);
    fadeInField.setText (juce::String::fromUTF8 (state.fadeInText.c_str()), juce::dontSendNotification);
    fadeOutField.setText (juce::String::fromUTF8 (state.fadeOutText.c_str()), juce::dontSendNotification);
    gainField.setText (juce::String::fromUTF8 (state.gainText.c_str()), juce::dontSendNotification);
    fadeInField.setEnabled (state.canEditFadesAndGain);
    fadeOutField.setEnabled (state.canEditFadesAndGain);
    gainField.setEnabled (state.canEditFadesAndGain);
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

//==============================================================================

EditPage::EditPage (PeaksLookup peaksFor)
    : timeline (std::move (peaksFor))
{
    toolbar.onSplit = [this] { emit (SongIntent { intent::SplitSelectedClip {} }); };
    toolbar.onDelete = [this] { emit (SongIntent { intent::DeleteSelection {} }); };
    toolbar.onZoomIn = [this] { emit (SongIntent { intent::SetTimelineView { timeline.view().firstSample, zoomedIn (timeline.view()).samplesPerPixel } }); };
    toolbar.onZoomOut = [this] { emit (SongIntent { intent::SetTimelineView { timeline.view().firstSample, zoomedOut (timeline.view()).samplesPerPixel } }); };
    toolbar.onToggleMixer = [this] { emit (SongIntent { intent::SetMixerVisible { ! mixerVisible } }); };
    toolbar.onFadeInEntered = [this] (const std::string& text) {
        const auto samples = parseSeconds (text, current.song.sampleRate);
        editSelectedClip ([&] (const song::Id& clipId) -> std::optional<intent::Edit> {
            if (! samples.has_value())
                return std::nullopt;
            return intent::SetClipFadeIn { clipId, *samples };
        });
    };
    toolbar.onFadeOutEntered = [this] (const std::string& text) {
        const auto samples = parseSeconds (text, current.song.sampleRate);
        editSelectedClip ([&] (const song::Id& clipId) -> std::optional<intent::Edit> {
            if (! samples.has_value())
                return std::nullopt;
            return intent::SetClipFadeOut { clipId, *samples };
        });
    };
    toolbar.onGainEntered = [this] (const std::string& text) {
        const auto gainDb = parseDecibels (text);
        editSelectedClip ([&] (const song::Id& clipId) -> std::optional<intent::Edit> {
            if (! gainDb.has_value())
                return std::nullopt;
            return intent::SetClipGain { clipId, *gainDb };
        });
    };

    timeline.onIntent = [this] (const Intent& intent) { emit (intent); };
    mixer.onIntent = [this] (const Intent& intent) { emit (intent); };
    mixer.onAddInsert = [this] (const song::Id& trackId) {
        if (onAddInsert)
            onAddInsert (trackId);
    };
    timeline.onDeleteTrack = [this] (const song::Id& trackId) {
        if (onDeleteTrack)
            onDeleteTrack (trackId);
    };
    timeline.onSelectionChanged = [this] { showToolbar(); };

    passesButton.setButtonText (juce::String::fromUTF8 ("Passes \xE2\x96\xBE"));
    passesButton.setWantsKeyboardFocus (false);
    passesButton.setColour (juce::TextButton::buttonOnColourId, colours::accent);
    passesButton.onClick = [this] { emit (SongIntent { intent::SetPassesPanelOpen { ! isPassesPanelOpen() } }); };
    passesPanel.onBringBack = [this] (const song::Id& passId) { bringBackPass (passId); };

    divider.onDragTo = [this] (int top) { emit (SongIntent { intent::SetMixerHeight { getHeight() - top - mixerDividerHeight } }); };

    addAndMakeVisible (toolbar);
    addAndMakeVisible (timeline);
    addAndMakeVisible (divider);
    addAndMakeVisible (mixer);
    addChildComponent (videoPreview);
    addAndMakeVisible (passesButton);
    addChildComponent (passesPanel);
    showToolbar();
    showPasses();
}

void EditPage::show (const TimelineState& state)
{
    // passRows walks every Pass: rebuild the panel only when what it shows can have changed.
    const auto passesChanged = state.song != current.song || state.editable != current.editable;
    current = state;
    timeline.show (current);
    mixer.show (current.song, current.editable);
    showToolbar();
    if (passesChanged)
        showPasses();
}

void EditPage::showPasses()
{
    const auto rowsBefore = passesPanel.numRows();
    passesPanel.show (passRows (current.song), current.editable);
    if (passesPanel.numRows() != rowsBefore)
        resized();
    passesButton.setToggleState (isPassesPanelOpen(), juce::dontSendNotification);
}

void EditPage::setPassesPanelOpen (bool open)
{
    passesPanel.setVisible (open);
    if (open)
        passesPanel.toFront (false);
    resized();
    showPasses();
}

void EditPage::bringBackPass (const song::Id& passId)
{
    if (! current.editable)
        return;
    // The button is enabled only for a Pass that can be brought back.
    if (! OVERDUB_CHECK (song::bringBackPass (current.song, passId).has_value(), "ui.bring_back_failed", { "pass_id", passId }))
        return;
    emit (SongIntent { intent::SetPassesPanelOpen { false } });
    emit (SongIntent { intent::Edit { intent::BringBackPass { passId } } });
}

void EditPage::showToolbar()
{
    const auto& videoTake = timeline.selectedVideoTake();
    const auto state = videoTake.has_value() ? videoTakeToolbarStateFor (current.song, current.videoTakes, *videoTake, current.editable)
                                             : clipToolbarStateFor (current.song, timeline.selectedClip(), current.playhead, current.editable);
    toolbar.show (state, mixerVisible);
}

void EditPage::showVideoPreview (bool armed, void* previewLayer, const juce::String& message, bool playback)
{
    videoPreview.show (previewLayer, message, playback);
    if (armed == cameraArmed)
        return;
    cameraArmed = armed;
    videoPreview.setVisible (isVideoPreviewShown());
    resized();
}

void EditPage::editSelectedClip (const std::function<std::optional<intent::Edit> (const song::Id& clipId)>& editFor)
{
    const auto& clipId = timeline.selectedClip();
    const auto edit = clipId.has_value() && current.editable ? editFor (*clipId) : std::nullopt;
    if (edit.has_value())
    {
        // The fields are enabled only while the selected Clip is in the Song.
        const auto changed = editedSong (current.song, *edit);
        if (OVERDUB_CHECK (changed.has_value(), "ui.clip_field_edit_failed", { "clip_id", *clipId }) && changed->song != current.song)
            emit (SongIntent { *edit });
    }
    showToolbar();
}

void EditPage::emit (Intent intent) const
{
    if (onIntent)
        onIntent (intent);
}

void EditPage::setMixerVisible (bool visible)
{
    mixerVisible = visible;
    divider.setVisible (visible);
    mixer.setVisible (visible);
    videoPreview.setVisible (isVideoPreviewShown());
    showToolbar();
    resized();
}

void EditPage::setMixerHeight (int height)
{
    mixerHeightRequested = clampMixerHeight (height, getHeight());
    resized();
}

void EditPage::resized()
{
    const auto layout = editPageLayout (getLocalBounds(), mixerVisible, mixerHeightRequested, cameraArmed);
    toolbar.setBounds (layout.clipToolbar);
    timeline.setBounds (layout.timeline);
    divider.setBounds (layout.mixerDivider);
    mixer.setBounds (layout.mixer);
    videoPreview.setBounds (layout.videoPreview);
    passesButton.setBounds (layout.passesButton);
    passesPanel.setBounds (passesPanelBounds (getLocalBounds(), layout.passesButton, passesPanel.numRows()));
}

void EditPage::paint (juce::Graphics& g)
{
    g.fillAll (colours::background);
}
} // namespace overdub::ui
