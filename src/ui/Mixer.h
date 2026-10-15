#pragma once

#include "core/Error.h"
#include "engine/RenderPlan.h"
#include "song/Song.h"
#include "ui/InsertChain.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Edit mode's Mixer (issue #31, issue #9 resolution, variant C): under the timeline, one strip per
// Track (name, pan, Insert Chain, fader with its meter, dB value, M and S) and the Master strip
// (name, Insert Chain, fader, meter, value) at the right edge. Thin over ui/MixerGeometry.h and
// song/MixEdits.h. The Insert Chains (issue #34, ui/InsertChain.h) report through insertActions.
//
// A fader or pan drag is previewed while it moves (onPreview: the Song as the drag would make it,
// heard through OpenSong::previewMix) and reported once, on mouse up, as one Song and its undo
// label (onEdit: "Guitar Volume", "Guitar Pan", "Master Volume"); a drag that ends where it
// started previews the Song as it was and reports nothing. Drags stick at 0 dB and C for
// detentPixels (MixerGeometry.h). A double-click sets 0 dB or C, one undo step with the same label
// (none when already there). M and S report at once ("Mute Guitar", "Solo Guitar").
//
// Keyboard focus (issue #129): grabbing a fader or pan focuses it (an accent outline); the window
// sends it keys first (handleKey). The arrows adjust it by faderKeyStepDb / panKeyStep, each press
// previewed at once; a run of presses on one control is one undo step, labelled like its drag,
// committed keyRunDuration after the last press (commitKeyRunIfDue), or sooner by commitKeyRun
// (focus moving or released, another Mixer gesture, or the window's own triggers). Esc releases
// focus. The focus is the Mixer's own state, not JUCE's keyboard focus, which stays on the window.
//
// While a Pass runs (not editable) no control acts and nothing has focus; the meters still move.
namespace overdub::ui
{
enum class MixerControlKind
{
    volume,
    pan
};

// One focusable control: a Track's fader or pan, or (empty trackId) the Master fader.
struct MixerControl
{
    song::Id trackId;
    MixerControlKind kind = MixerControlKind::volume;

    bool operator== (const MixerControl&) const = default;
};

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

    static constexpr int thumbSize = 10;

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

// A vertical peak meter on the fader's scale (meterPosition).
class LevelMeter final : public juce::Component
{
public:
    // Takes a poll's peak; the shown level falls back slowly (nextMeterLevel).
    void addPeak (float peak);
    float level() const { return shownLevel; }

    void paint (juce::Graphics& g) override;

private:
    float shownLevel = 0.0f;
};

struct MixerStripState
{
    song::Id trackId; // empty for the Master
    std::string name;
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
    using Clock = std::chrono::steady_clock;
    // How long after the last arrow press a run of presses becomes its undo step.
    static constexpr auto keyRunDuration = std::chrono::seconds (1);

    Mixer();

    // The Song whose mix the strips show; not editable while a Pass runs.
    void show (const song::Song& song, bool editable);
    // One poll's peaks (Engine::takeMeterPeaks); Tracks it does not name read as silent.
    void showMeters (const engine::MeterPeaks& peaks);
    void resized() override;
    void paint (juce::Graphics& g) override;

    // Keyboard focus: the control that takes the arrow keys, if any.
    const std::optional<MixerControl>& focusedControl() const { return focused; }
    // True when `component` is the focused control or inside it.
    bool isFocusedControl (const juce::Component* component);
    // Focuses a control (committing a run on another one).
    void focus (const MixerControl& control);
    // Commits the run, if any, and leaves no control focused.
    void releaseFocus();
    // A key press while a control has focus: the arrows adjust it, Esc releases it. Returns
    // whether the key was used; every other key is left to the window.
    bool handleKey (const juce::KeyPress& key, Clock::time_point now);

    bool hasKeyRun() const { return keyRun.has_value(); }
    // Reports the run of arrow presses (if any) as one undo step now.
    void commitKeyRun();
    // Commits the run once keyRunDuration has passed since its last press.
    void commitKeyRunIfDue (Clock::time_point now);
    // An edit made from the Song before the run was committed, with the run's value carried into
    // it, unless the edit set that control itself (or removed its Track).
    song::Song withKeyRunCarried (song::Song changed) const;

    // The Song a gesture in progress would make (the mix only).
    std::function<void (const song::Song& preview)> onPreview;
    // A finished gesture: the changed Song and its undo label.
    std::function<void (song::Song changed, std::string label)> onEdit;
    // The Insert Chains' gestures (every strip's, the Master's included).
    InsertChainActions insertActions;
    // Each slot's plugin status, for the chips; none: every plugin reads as ready.
    InsertStatusLookup insertStatusOf;

    std::vector<std::unique_ptr<MixerStrip>> strips;
    MixerStrip masterStrip { true };

private:
    struct KeyRun
    {
        MixerControl control;
        double value = 0.0;
        Clock::time_point deadline;
    };

    void rebuildStrips();
    void connectStrip (MixerStrip& strip, const song::Id& trackId);
    MixerStrip* stripFor (const song::Id& trackId);
    double shownValue (const MixerControl& control, double songValue) const;
    void showFocus();
    void preview (core::Expected<song::Song> candidate);
    void report (core::Expected<song::Song> changed, const std::string& label);

    song::Song current;
    bool editable = true;
    std::optional<MixerControl> focused;
    std::optional<KeyRun> keyRun;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Mixer)
};
} // namespace overdub::ui
