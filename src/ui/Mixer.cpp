#include "ui/Mixer.h"

#include "song/MixEdits.h"
#include "song/TakeEdits.h"
#include "ui/Colours.h"
#include "ui/Layout.h"
#include "ui/MixerGeometry.h"
#include "ui/MuteSoloButtons.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace overdub::ui
{
//==============================================================================

namespace
{
void paintFocusOutline (juce::Graphics& g, const juce::Component& control)
{
    g.setColour (colours::accent);
    g.drawRoundedRectangle (control.getLocalBounds().toFloat().reduced (0.5f), 3.0f, 1.0f);
}
} // namespace

//==============================================================================

MixerFader::MixerFader (Orientation faderOrientation) : orientation (faderOrientation)
{
}

int MixerFader::travel() const
{
    const auto length = orientation == Orientation::vertical ? getHeight() : getWidth();
    return std::max (0, length - thumbSize);
}

juce::Point<int> MixerFader::thumbCentre() const
{
    const auto position = dbToFaderPosition (shownDb);
    if (orientation == Orientation::vertical)
        return { getWidth() / 2, thumbSize / 2 + (int) std::lround ((1.0 - position) * travel()) };
    return { thumbSize / 2 + (int) std::lround (position * travel()), getHeight() / 2 };
}

void MixerFader::setDb (double db)
{
    if (dragging || juce::exactlyEqual (db, shownDb))
        return;
    shownDb = db;
    repaint();
}

void MixerFader::setFocusShown (bool shown)
{
    if (shown == focusShown)
        return;
    focusShown = shown;
    repaint();
}

void MixerFader::mouseDown (const juce::MouseEvent& event)
{
    if (! isEnabled())
        return;
    if (onGrab)
        onGrab();
    dragging = true;
    dragStartDb = shownDb;
    dragStart = event.getPosition();
}

void MixerFader::mouseDrag (const juce::MouseEvent& event)
{
    if (! dragging)
        return;
    const auto moved = event.getPosition() - dragStart;
    const auto louderPixels = orientation == Orientation::vertical ? -moved.y : moved.x;
    const auto db = faderDragDb (dragStartDb, louderPixels, travel());
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

void MixerFader::mouseDoubleClick (const juce::MouseEvent&)
{
    if (isEnabled() && onReset)
        onReset();
}

void MixerFader::paint (juce::Graphics& g)
{
    // Drawn along the travel axis: `along` runs from the quiet end, `across` is the other axis.
    const auto vertical = orientation == Orientation::vertical;
    const auto across = (float) (vertical ? getWidth() : getHeight()) / 2.0f;
    const auto half = (float) thumbSize / 2.0f;
    const auto alongFor = [this, half, vertical] (double db) {
        const auto position = dbToFaderPosition (db);
        return half + (float) ((vertical ? 1.0 - position : position) * travel());
    };
    const auto rect = [vertical] (float along, float acrossStart, float alongLength, float acrossLength) {
        return vertical ? juce::Rectangle<float> (acrossStart, along, acrossLength, alongLength)
                        : juce::Rectangle<float> (along, acrossStart, alongLength, acrossLength);
    };

    g.setColour (colours::background);
    g.fillRoundedRectangle (rect (half, across - 2.0f, (float) travel(), 4.0f), 2.0f);
    g.setColour (colours::dimText);
    const auto unity = (float) (int) alongFor (0.0); // 0 dB ticks either side of the track
    g.fillRect (rect (unity, across - 7.0f, 1.0f, 4.0f));
    g.fillRect (rect (unity, across + 3.0f, 1.0f, 4.0f));

    const auto thumbLength = vertical ? (float) getWidth() : (float) getHeight();
    const auto thumb = rect (alongFor (shownDb) - half, 0.0f, (float) thumbSize, thumbLength).reduced (vertical ? 1.0f : 0.0f, vertical ? 0.0f : 1.0f);
    g.setColour (isEnabled() ? colours::text : colours::dimText);
    g.fillRoundedRectangle (thumb, 2.0f);
    g.setColour (colours::background);
    if (vertical)
        g.drawHorizontalLine ((int) thumb.getCentreY(), thumb.getX() + 3.0f, thumb.getRight() - 3.0f);
    else
        g.drawVerticalLine ((int) thumb.getCentreX(), thumb.getY() + 3.0f, thumb.getBottom() - 3.0f);

    if (focusShown)
        paintFocusOutline (g, *this);
}

//==============================================================================

void PanControl::setPan (double pan)
{
    if (dragging || juce::exactlyEqual (pan, shownPan))
        return;
    shownPan = pan;
    repaint();
}

void PanControl::setFocusShown (bool shown)
{
    if (shown == focusShown)
        return;
    focusShown = shown;
    repaint();
}

void PanControl::mouseDown (const juce::MouseEvent& event)
{
    if (! isEnabled())
        return;
    if (onGrab)
        onGrab();
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

void PanControl::mouseDoubleClick (const juce::MouseEvent&)
{
    if (isEnabled() && onReset)
        onReset();
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

    if (focusShown)
        paintFocusOutline (g, *this);
}

//==============================================================================

LevelMeter::LevelMeter (MixerFader::Orientation meterOrientation) : orientation (meterOrientation)
{
}

void LevelMeter::addPeak (float peak)
{
    const auto next = nextMeterLevel (shownLevel, peak);
    if (juce::exactlyEqual (next, shownLevel))
        return;
    shownLevel = next;
    repaint();
}

juce::Rectangle<float> LevelMeter::litArea() const
{
    const auto area = getLocalBounds().toFloat();
    const auto position = (float) meterPosition (shownLevel);
    if (position <= 0.0f)
        return {};
    if (orientation == MixerFader::Orientation::vertical)
        return area.withTop (area.getBottom() - position * area.getHeight());
    return area.withWidth (position * area.getWidth());
}

void LevelMeter::paint (juce::Graphics& g)
{
    g.setColour (colours::background.darker (0.4f));
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 2.0f);

    const auto lit = litArea();
    if (lit.isEmpty())
        return;
    const auto position = (float) meterPosition (shownLevel);
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

    setUpMuteSoloButtons (muteButton, soloButton, [this] { return current.mute; }, [this] { return current.solo; }, onMute, onSolo);

    fader.onGrab = [this] {
        if (onGrab)
            onGrab (MixerControlKind::volume);
    };
    fader.onDrag = [this] (double db) {
        showValue();
        if (onDrag)
            onDrag (MixerControlKind::volume, db);
    };
    fader.onRelease = [this] (double startDb, double endDb) {
        showValue();
        release (MixerControlKind::volume, startDb, endDb);
    };
    fader.onReset = [this] {
        if (onReset)
            onReset (MixerControlKind::volume);
    };
    panControl.onGrab = [this] {
        if (onGrab)
            onGrab (MixerControlKind::pan);
    };
    panControl.onDrag = [this] (double pan) {
        if (onDrag)
            onDrag (MixerControlKind::pan, pan);
    };
    panControl.onRelease = [this] (double startPan, double endPan) { release (MixerControlKind::pan, startPan, endPan); };
    panControl.onReset = [this] {
        if (onReset)
            onReset (MixerControlKind::pan);
    };

    addAndMakeVisible (nameLabel);
    addAndMakeVisible (insertChain);
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

void MixerStrip::release (MixerControlKind kind, double startValue, double endValue)
{
    if (juce::exactlyEqual (startValue, endValue))
    {
        if (onGestureAbandoned)
            onGestureAbandoned();
    }
    else if (onChanged)
        onChanged (kind, endValue);
}

void MixerStrip::show (const MixerStripState& state)
{
    current = state;
    nameLabel.setText (juce::String::fromUTF8 (state.name.c_str()), juce::dontSendNotification);
    fader.setDb (state.volumeDb);
    panControl.setPan (state.pan);
    muteButton.setToggleState (state.mute, juce::dontSendNotification);
    soloButton.setToggleState (state.solo, juce::dontSendNotification);
    insertChain.show (state.trackId, state.inserts, state.enabled);
    for (auto* control : std::initializer_list<juce::Component*> { &fader, &panControl, &muteButton, &soloButton })
        control->setEnabled (state.enabled);
    showValue();
    repaint();
}

void MixerStrip::showControlValue (MixerControlKind kind, double value)
{
    if (kind == MixerControlKind::volume)
        fader.setDb (value);
    else
        panControl.setPan (value);
    showValue();
}

juce::Component& MixerStrip::control (MixerControlKind kind)
{
    if (kind == MixerControlKind::volume)
        return fader;
    return panControl;
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
    insertChain.setBounds (layout.inserts);
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
    if (current.colour.has_value())
    {
        juce::Graphics::ScopedSaveState saved (g);
        g.reduceClipRegion (getLocalBounds().removeFromTop (trackColourStripeWidth));
        g.setColour (*current.colour);
        g.fillRoundedRectangle (area, 6.0f);
    }
    g.setColour (colours::line);
    g.drawRoundedRectangle (area, 6.0f, 1.0f);
}

//==============================================================================

Mixer::Mixer()
{
    mix.onFocusChanged = [this] { showFocus(); };
    mix.onKeyValue = [this] (const MixerControl& control, double value) {
        if (auto* strip = stripFor (control.trackId))
            strip->showControlValue (control.kind, value);
    };
    masterStrip.insertChain.actions = &insertActions;
    connectStrip (masterStrip, {});
    addAndMakeVisible (masterStrip);
}

void Mixer::show (const song::Song& song, bool isEditable)
{
    const auto sameTracks = std::ranges::equal (strips, song.tracks, {}, &MixerStrip::trackId, &song::Track::id);
    mix.show (song, isEditable);
    if (! sameTracks)
        rebuildStrips();

    const auto& current = mix.song();
    const auto editable = mix.isEditable();
    for (std::size_t i = 0; i < strips.size(); ++i)
    {
        const auto& track = current.tracks[i];
        strips[i]->show ({ .trackId = track.id,
                           .name = track.name,
                           .colour = trackColour (track),
                           .volumeDb = mix.shownValue ({ track.id, MixerControlKind::volume }, track.volumeDb),
                           .pan = mix.shownValue ({ track.id, MixerControlKind::pan }, track.pan),
                           .mute = track.mute,
                           .solo = track.solo,
                           .inserts = insertChipsFor (track.insertChain, insertStatusOf),
                           .enabled = editable });
    }
    masterStrip.show ({ .trackId = {},
                        .name = "Master",
                        .volumeDb = mix.shownValue ({ {}, MixerControlKind::volume }, current.master.volumeDb),
                        .inserts = insertChipsFor (current.master.insertChain, insertStatusOf),
                        .enabled = editable });
    showFocus();
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

MixerStrip* Mixer::stripFor (const song::Id& trackId)
{
    if (trackId.empty())
        return &masterStrip;
    for (auto& strip : strips)
        if (strip->trackId() == trackId)
            return strip.get();
    return nullptr;
}

bool Mixer::isFocusedControl (const juce::Component* component)
{
    const auto& focused = mix.focusedControl();
    if (! focused.has_value() || component == nullptr)
        return false;
    auto* strip = stripFor (focused->trackId);
    if (strip == nullptr)
        return false;
    auto& control = strip->control (focused->kind);
    return component == &control || control.isParentOf (component);
}

void Mixer::showFocus()
{
    const auto& focused = mix.focusedControl();
    masterStrip.fader.setFocusShown (focused == MixerControl { {}, MixerControlKind::volume });
    for (auto& strip : strips)
    {
        strip->fader.setFocusShown (focused == MixerControl { strip->trackId(), MixerControlKind::volume });
        strip->panControl.setFocusShown (focused == MixerControl { strip->trackId(), MixerControlKind::pan });
    }
}

void Mixer::rebuildStrips()
{
    strips.clear();
    for (const auto& track : mix.song().tracks)
    {
        auto strip = std::make_unique<MixerStrip> (false);
        strip->insertChain.actions = &insertActions;
        connectStrip (*strip, track.id);
        addAndMakeVisible (*strip);
        strips.push_back (std::move (strip));
    }
    resized();
}

void Mixer::connectStrip (MixerStrip& strip, const song::Id& trackId)
{
    strip.onGrab = [this, trackId] (MixerControlKind kind) { mix.focus ({ trackId, kind }); };
    strip.onDrag = [this, trackId] (MixerControlKind kind, double value) { mix.drag ({ trackId, kind }, value); };
    strip.onChanged = [this, trackId] (MixerControlKind kind, double value) { mix.set ({ trackId, kind }, value); };
    strip.onReset = [this, trackId] (MixerControlKind kind) { mix.set ({ trackId, kind }, 0.0); };
    strip.onGestureAbandoned = [this] { mix.abandon(); };
    // M and S build on a pending burst: it is committed before the Song they change is read.
    strip.onMute = [this, trackId] (bool mute) {
        mix.commitKeyBurst();
        mix.report (intent::SetTrackMute { trackId, mute });
    };
    strip.onSolo = [this, trackId] (bool solo) {
        mix.commitKeyBurst();
        mix.report (intent::SetTrackSolo { trackId, solo });
    };
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
