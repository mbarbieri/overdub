#include "ui/Mixer.h"

#include "song/MixEdits.h"
#include "ui/Colours.h"
#include "ui/Layout.h"
#include "ui/MixerGeometry.h"

#include <algorithm>

namespace overdub::ui
{
//==============================================================================

void MixerFader::setDb (double db)
{
    if (dragging || juce::exactlyEqual (db, shownDb))
        return;
    shownDb = db;
    repaint();
}

void MixerFader::mouseDown (const juce::MouseEvent& event)
{
    if (! isEnabled())
        return;
    dragging = true;
    dragStartDb = shownDb;
    dragStartY = event.getPosition().y;
}

void MixerFader::mouseDrag (const juce::MouseEvent& event)
{
    if (! dragging)
        return;
    const auto db = faderDragDb (dragStartDb, event.getPosition().y - dragStartY, travel());
    if (juce::exactlyEqual (db, shownDb))
        return;
    shownDb = db;
    repaint();
    if (onDrag)
        onDrag (db);
}

void MixerFader::mouseUp (const juce::MouseEvent&)
{
    if (! dragging)
        return;
    dragging = false;
    if (onRelease)
        onRelease (dragStartDb, shownDb);
}

void MixerFader::paint (juce::Graphics& g)
{
    const auto centreX = (float) getWidth() / 2.0f;
    const auto half = (float) thumbHeight / 2.0f;
    const auto yFor = [this, half] (double db) { return half + (float) ((1.0 - dbToFaderPosition (db)) * travel()); };

    g.setColour (colours::background);
    g.fillRoundedRectangle (centreX - 2.0f, half, 4.0f, (float) travel(), 2.0f);
    g.setColour (colours::dimText);
    g.drawHorizontalLine ((int) yFor (0.0), centreX - 7.0f, centreX - 3.0f); // 0 dB
    g.drawHorizontalLine ((int) yFor (0.0), centreX + 3.0f, centreX + 7.0f);

    const auto thumb = juce::Rectangle<float> (0.0f, yFor (shownDb) - half, (float) getWidth(), (float) thumbHeight).reduced (1.0f, 0.0f);
    g.setColour (isEnabled() ? colours::text : colours::dimText);
    g.fillRoundedRectangle (thumb, 2.0f);
    g.setColour (colours::background);
    g.drawHorizontalLine ((int) thumb.getCentreY(), thumb.getX() + 3.0f, thumb.getRight() - 3.0f);
}

//==============================================================================

void PanControl::setPan (double pan)
{
    if (dragging || juce::exactlyEqual (pan, shownPan))
        return;
    shownPan = pan;
    repaint();
}

void PanControl::mouseDown (const juce::MouseEvent& event)
{
    if (! isEnabled())
        return;
    dragging = true;
    dragStartPan = shownPan;
    dragStartX = event.getPosition().x;
}

void PanControl::mouseDrag (const juce::MouseEvent& event)
{
    if (! dragging)
        return;
    const auto pan = panDrag (dragStartPan, event.getPosition().x - dragStartX, getWidth());
    if (juce::exactlyEqual (pan, shownPan))
        return;
    shownPan = pan;
    repaint();
    if (onDrag)
        onDrag (pan);
}

void PanControl::mouseUp (const juce::MouseEvent&)
{
    if (! dragging)
        return;
    dragging = false;
    if (onRelease)
        onRelease (dragStartPan, shownPan);
}

void PanControl::paint (juce::Graphics& g)
{
    const auto area = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (colours::background);
    g.fillRoundedRectangle (area, 3.0f);

    // A bar from the centre to the pan position, and the value over it.
    const auto centreX = area.getCentreX();
    const auto x = area.getX() + (float) panPosition (shownPan) * area.getWidth();
    g.setColour ((isEnabled() ? colours::accent : colours::dimText).withAlpha (0.7f));
    g.fillRect (juce::Rectangle<float>::leftTopRightBottom (std::min (centreX, x), area.getY() + 2.0f, std::max (centreX, x) + 1.0f, area.getBottom() - 2.0f));
    g.setColour (colours::line);
    g.drawVerticalLine ((int) centreX, area.getY(), area.getBottom());
    g.setColour (colours::text);
    g.setFont (juce::FontOptions (10.5f));
    g.drawText (juce::String (panText (shownPan)), getLocalBounds(), juce::Justification::centred);
}

//==============================================================================

void LevelMeter::addPeak (float peak)
{
    const auto next = nextMeterLevel (shownLevel, peak);
    if (juce::exactlyEqual (next, shownLevel))
        return;
    shownLevel = next;
    repaint();
}

void LevelMeter::paint (juce::Graphics& g)
{
    const auto area = getLocalBounds().toFloat();
    g.setColour (colours::background.darker (0.4f));
    g.fillRoundedRectangle (area, 2.0f);

    const auto position = (float) meterPosition (shownLevel);
    if (position <= 0.0f)
        return;
    const auto lit = area.withTop (area.getBottom() - position * area.getHeight());
    const auto colour = position > 0.75f ? colours::rec : (position > 0.66f ? colours::solo : juce::Colour (0xff3fbf6f));
    g.setColour (colour);
    g.fillRoundedRectangle (lit, 2.0f);
}

//==============================================================================

MixerStrip::MixerStrip (bool isMaster) : master (isMaster)
{
    nameLabel.setColour (juce::Label::textColourId, colours::text);
    nameLabel.setFont (juce::FontOptions (12.0f, juce::Font::bold));
    nameLabel.setJustificationType (juce::Justification::centred);
    nameLabel.setInterceptsMouseClicks (false, false);
    valueLabel.setColour (juce::Label::textColourId, colours::dimText);
    valueLabel.setFont (juce::FontOptions (10.5f));
    valueLabel.setJustificationType (juce::Justification::centred);
    valueLabel.setInterceptsMouseClicks (false, false);

    muteButton.setColour (juce::TextButton::buttonOnColourId, colours::mute);
    soloButton.setColour (juce::TextButton::buttonOnColourId, colours::solo);
    soloButton.setColour (juce::TextButton::textColourOnId, colours::background);
    for (auto* button : { &muteButton, &soloButton })
        button->setWantsKeyboardFocus (false);
    muteButton.onClick = [this] {
        if (onMute)
            onMute (! current.mute);
    };
    soloButton.onClick = [this] {
        if (onSolo)
            onSolo (! current.solo);
    };

    fader.onDrag = [this] (double db) {
        showValue();
        if (onVolumeDrag)
            onVolumeDrag (db);
    };
    fader.onRelease = [this] (double startDb, double endDb) {
        showValue();
        if (juce::exactlyEqual (startDb, endDb))
        {
            if (onGestureAbandoned)
                onGestureAbandoned();
        }
        else if (onVolumeChanged)
            onVolumeChanged (endDb);
    };
    panControl.onDrag = [this] (double pan) {
        if (onPanDrag)
            onPanDrag (pan);
    };
    panControl.onRelease = [this] (double startPan, double endPan) {
        if (juce::exactlyEqual (startPan, endPan))
        {
            if (onGestureAbandoned)
                onGestureAbandoned();
        }
        else if (onPanChanged)
            onPanChanged (endPan);
    };

    addAndMakeVisible (nameLabel);
    addAndMakeVisible (fader);
    addAndMakeVisible (meter);
    addAndMakeVisible (valueLabel);
    if (! master)
    {
        addAndMakeVisible (panControl);
        addAndMakeVisible (muteButton);
        addAndMakeVisible (soloButton);
    }
}

void MixerStrip::show (const MixerStripState& state)
{
    current = state;
    nameLabel.setText (juce::String::fromUTF8 (state.name.c_str()), juce::dontSendNotification);
    fader.setDb (state.volumeDb);
    panControl.setPan (state.pan);
    muteButton.setToggleState (state.mute, juce::dontSendNotification);
    soloButton.setToggleState (state.solo, juce::dontSendNotification);
    for (auto* control : std::initializer_list<juce::Component*> { &fader, &panControl, &muteButton, &soloButton })
        control->setEnabled (state.enabled);
    showValue();
    repaint();
}

void MixerStrip::showValue()
{
    valueLabel.setText (juce::String (volumeText (fader.db())), juce::dontSendNotification);
}

void MixerStrip::resized()
{
    const auto layout = mixerStripLayout (getLocalBounds());
    nameLabel.setBounds (layout.name);
    panControl.setBounds (layout.pan);
    fader.setBounds (layout.fader);
    meter.setBounds (layout.meter);
    valueLabel.setBounds (layout.value);
    muteButton.setBounds (layout.mute);
    soloButton.setBounds (layout.solo);
}

void MixerStrip::paint (juce::Graphics& g)
{
    const auto area = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (master ? juce::Colour (0xff2b2a33) : colours::panel);
    g.fillRoundedRectangle (area, 6.0f);
    g.setColour (colours::line);
    g.drawRoundedRectangle (area, 6.0f, 1.0f);
}

//==============================================================================

Mixer::Mixer()
{
    connectStrip (masterStrip, {});
    addAndMakeVisible (masterStrip);
}

void Mixer::show (const song::Song& song, bool isEditable)
{
    const auto sameTracks = std::ranges::equal (strips, song.tracks, {}, &MixerStrip::trackId, &song::Track::id);
    current = song;
    editable = isEditable;
    if (! sameTracks)
        rebuildStrips();

    for (std::size_t i = 0; i < strips.size(); ++i)
    {
        const auto& track = current.tracks[i];
        strips[i]->show ({ .trackId = track.id, .name = track.name, .volumeDb = track.volumeDb, .pan = track.pan, .mute = track.mute, .solo = track.solo, .enabled = editable });
    }
    masterStrip.show ({ .trackId = {}, .name = "Master", .volumeDb = current.master.volumeDb, .enabled = editable });
}

void Mixer::showMeters (const engine::MeterPeaks& peaks)
{
    for (auto& strip : strips)
    {
        const auto found = peaks.tracks.find (strip->trackId());
        strip->meter.addPeak (found != peaks.tracks.end() ? found->second : 0.0f);
    }
    masterStrip.meter.addPeak (peaks.master);
}

void Mixer::rebuildStrips()
{
    strips.clear();
    for (const auto& track : current.tracks)
    {
        auto strip = std::make_unique<MixerStrip> (false);
        connectStrip (*strip, track.id);
        addAndMakeVisible (*strip);
        strips.push_back (std::move (strip));
    }
    resized();
}

void Mixer::connectStrip (MixerStrip& strip, const song::Id& trackId)
{
    // Every gesture starts from `current`, the Song as shown: a drag's preview never accumulates.
    const auto isMaster = trackId.empty();
    const auto withVolume = [this, trackId, isMaster] (double db) -> core::Expected<song::Song> {
        if (isMaster)
            return song::setMasterVolume (current, db);
        return song::setTrackVolume (current, trackId, db);
    };

    strip.onVolumeDrag = [this, withVolume] (double db) {
        if (auto preview = withVolume (db); preview.has_value() && onPreview)
            onPreview (*preview);
    };
    strip.onVolumeChanged = [this, withVolume, trackId, isMaster] (double db) {
        report (withVolume (db), isMaster ? std::string (song::masterVolumeLabel) : song::volumeLabel (current, trackId));
    };
    strip.onPanDrag = [this, trackId] (double pan) {
        if (auto preview = song::setTrackPan (current, trackId, pan); preview.has_value() && onPreview)
            onPreview (*preview);
    };
    strip.onPanChanged = [this, trackId] (double pan) {
        report (song::setTrackPan (current, trackId, pan), song::panLabel (current, trackId));
    };
    strip.onGestureAbandoned = [this] {
        if (onPreview)
            onPreview (current);
    };
    strip.onMute = [this, trackId] (bool mute) {
        report (song::setTrackMute (current, trackId, mute), song::muteLabel (current, trackId, mute));
    };
    strip.onSolo = [this, trackId] (bool solo) {
        report (song::setTrackSolo (current, trackId, solo), song::soloLabel (current, trackId, solo));
    };
}

void Mixer::report (core::Expected<song::Song> changed, const std::string& label)
{
    // The strips exist only for the shown Song's Tracks, so the Track is there.
    if (! editable || ! OVERDUB_CHECK (changed.has_value(), "ui.mixer_edit_failed", { "label", label }))
        return;
    if (*changed != current && onEdit)
        onEdit (std::move (*changed), label);
}

void Mixer::resized()
{
    const auto layout = mixerLayout (getLocalBounds(), (int) strips.size());
    for (std::size_t i = 0; i < strips.size(); ++i)
    {
        strips[i]->setBounds (layout.strips[i]);
        strips[i]->setVisible (! layout.strips[i].isEmpty());
    }
    masterStrip.setBounds (layout.master);
}

void Mixer::paint (juce::Graphics& g)
{
    g.fillAll (colours::panel.darker (0.2f));
}
} // namespace overdub::ui
