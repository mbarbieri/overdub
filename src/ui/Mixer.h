#pragma once

#include "core/Error.h"
#include "engine/RenderPlan.h"
#include "song/Song.h"
#include "ui/InsertChain.h"
#include "ui/Layout.h"
#include "ui/MixFocus.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

// Edit mode's Mixer (issue #31, issue #9 resolution, variant C): under the timeline, one strip per
// Track (name, pan, Insert Chain, fader with its meter, dB value, M and S) and the Master strip
// (name, Insert Chain, fader, meter, value) at the right edge. Thin over ui/MixerGeometry.h and
// song/MixEdits.h. Every gesture that changes state is an Intent reported through onIntent: the
// controls' (MixFocus), M and S, and the Insert Chains' (issue #34, ui/InsertChain.h), whose
// "+ Insert" is the one hook, onAddInsert (the window asks, then runs add_insert).
//
// The controls' behaviour (drag previews, undo steps, double-click, keyboard focus and the arrow
// keys' bursts) is the Mixer's MixFocus (ui/MixFocus.h); the strips draw it and report gestures.
// Drags stick at 0 dB and C for detentPixels (MixerGeometry.h). M and S report at once ("Mute
// Guitar", "Solo Guitar").
//
// While a Pass runs (not editable) no control acts and nothing has focus; the meters still move.
namespace overdub::ui
{
// A fader: dragging moves it relative to where it was grabbed (faderDragDb), up or right is
// louder. Its behaviour is the same in both orientations.
class MixerFader final : public juce::Component
{
public:
    enum class Orientation
    {
        vertical,
        horizontal
    };

    static constexpr int thumbSize = faderThumbSize;

    explicit MixerFader (Orientation orientation = Orientation::vertical);

    // Shows this value, unless a drag is in progress (the drag's value stays).
    void setDb (double db);
    double db() const { return shownDb; }
    bool isDragging() const { return dragging; }
    // The pixels the thumb travels from -60 dB to +12 dB.
    int travel() const;
    // The thumb's centre, in the fader's coordinates.
    juce::Point<int> thumbCentre() const;
    void setFocusShown (bool shown);
    bool isFocusShown() const { return focusShown; }

    std::function<void()> onGrab;
    std::function<void (double db)> onDrag;
    std::function<void (double startDb, double endDb)> onRelease;
    std::function<void()> onReset;

    void mouseDown (const juce::MouseEvent& event) override;
    void mouseDrag (const juce::MouseEvent& event) override;
    void mouseUp (const juce::MouseEvent& event) override;
    void mouseDoubleClick (const juce::MouseEvent& event) override;
    void paint (juce::Graphics& g) override;

private:
    Orientation orientation;
    double shownDb = 0.0;
    double dragStartDb = 0.0;
    juce::Point<int> dragStart;
    bool dragging = false;
    bool focusShown = false;
};

// A horizontal pan control: dragging moves pan relative to where it was grabbed (panDrag).
class PanControl final : public juce::Component
{
public:
    void setPan (double pan);
    double pan() const { return shownPan; }
    bool isDragging() const { return dragging; }
    void setFocusShown (bool shown);
    bool isFocusShown() const { return focusShown; }

    std::function<void()> onGrab;
    std::function<void (double pan)> onDrag;
    std::function<void (double startPan, double endPan)> onRelease;
    std::function<void()> onReset;

    void mouseDown (const juce::MouseEvent& event) override;
    void mouseDrag (const juce::MouseEvent& event) override;
    void mouseUp (const juce::MouseEvent& event) override;
    void mouseDoubleClick (const juce::MouseEvent& event) override;
    void paint (juce::Graphics& g) override;

private:
    double shownPan = 0.0;
    double dragStartPan = 0.0;
    int dragStartX = 0;
    bool dragging = false;
    bool focusShown = false;
};

// A peak meter on the fader's scale (meterPosition): vertical (lit from the bottom) on a Mixer
// strip, horizontal (lit from the left) under a Record row's fader.
class LevelMeter final : public juce::Component
{
public:
    explicit LevelMeter (MixerFader::Orientation orientation = MixerFader::Orientation::vertical);

    // Takes a poll's peak; the shown level falls back slowly (nextMeterLevel).
    void addPeak (float peak);
    float level() const { return shownLevel; }
    // The lit part for the shown level, in the meter's coordinates; empty when silent.
    juce::Rectangle<float> litArea() const;

    void paint (juce::Graphics& g) override;

private:
    MixerFader::Orientation orientation;
    float shownLevel = 0.0f;
};

struct MixerStripState
{
    song::Id trackId; // empty for the Master
    std::string name;
    std::optional<juce::Colour> colour; // the Track's (ui/Colours.h), a band along the strip's top; none for the Master
    double volumeDb = 0.0;
    double pan = 0.0;
    bool mute = false;
    bool solo = false;
    std::vector<InsertChipState> inserts;
    bool enabled = true;
};

class MixerStrip final : public juce::Component
{
public:
    explicit MixerStrip (bool isMaster);

    void show (const MixerStripState& state);
    // Shows a value a key press set, ahead of the Song.
    void showControlValue (MixerControlKind kind, double value);
    void resized() override;
    void paint (juce::Graphics& g) override;

    const song::Id& trackId() const { return current.trackId; }
    const std::optional<juce::Colour>& colour() const { return current.colour; }
    juce::Component& control (MixerControlKind kind);

    std::function<void (MixerControlKind kind)> onGrab;
    std::function<void (MixerControlKind kind, double value)> onDrag;
    std::function<void (MixerControlKind kind, double value)> onChanged;
    std::function<void (MixerControlKind kind)> onReset;
    // A drag that ended where it started.
    std::function<void()> onGestureAbandoned;
    std::function<void (bool mute)> onMute;
    std::function<void (bool solo)> onSolo;

    juce::Label nameLabel;
    PanControl panControl;
    InsertChainView insertChain { true };
    MixerFader fader;
    LevelMeter meter;
    juce::Label valueLabel;
    juce::TextButton muteButton { "M" };
    juce::TextButton soloButton { "S" };

private:
    void showValue();
    void release (MixerControlKind kind, double startValue, double endValue);

    bool master;
    MixerStripState current;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixerStrip)
};

class Mixer final : public juce::Component
{
public:
    Mixer();

    // The Song whose mix the strips show; not editable while a Pass runs.
    void show (const song::Song& song, bool editable);
    // One poll's peaks (Engine::takeMeterPeaks); Tracks it does not name read as silent.
    void showMeters (const engine::MeterPeaks& peaks);
    void resized() override;
    void paint (juce::Graphics& g) override;

    // True when `component` is the focused control or inside it.
    bool isFocusedControl (const juce::Component* component);

    // Where the Mixer's Intents go (the Edit page's sink).
    IntentSink onIntent;
    // "+ Insert" on a strip's chain (empty Track id: the Master's).
    std::function<void (const song::Id& trackId)> onAddInsert;
    // Each slot's plugin status, for the chips; none: every plugin reads as ready.
    InsertStatusLookup insertStatusOf;

    // The controls' focus, arrow keys and undo steps (their Intents run through the window).
    MixFocus mix;
    std::vector<std::unique_ptr<MixerStrip>> strips;
    MixerStrip masterStrip { true };

private:
    void rebuildStrips();
    void connectStrip (MixerStrip& strip, const song::Id& trackId);
    void connectChain (InsertChainView& chain);
    void emit (Intent intent) const;
    MixerStrip* stripFor (const song::Id& trackId);
    void showFocus();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Mixer)
};
} // namespace overdub::ui
