#pragma once

#include "core/Error.h"
#include "engine/RenderPlan.h"
#include "song/Song.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

// Edit mode's Mixer (issue #31, issue #9 resolution, variant C): under the timeline, one strip per
// Track (name, pan, fader with its meter, dB value, M and S) and the Master strip (name, fader,
// meter, value) at the right edge. Thin over ui/MixerGeometry.h and song/MixEdits.h.
//
// A fader or pan drag is previewed while it moves (onPreview: the Song as the drag would make it,
// heard through OpenSong::previewMix) and reported once, on mouse up, as one Song and its undo
// label (onEdit: "Guitar Volume", "Guitar Pan", "Master Volume"); a drag that ends where it
// started previews the Song as it was and reports nothing. M and S report at once ("Mute Guitar",
// "Solo Guitar"). While a Pass runs (not editable) no control acts; the meters still move.
namespace overdub::ui
{
// A vertical fader: dragging moves it relative to where it was grabbed (faderDragDb).
class MixerFader final : public juce::Component
{
public:
    static constexpr int thumbHeight = 10;

    // Shows this value, unless a drag is in progress (the drag's value stays).
    void setDb (double db);
    double db() const { return shownDb; }
    bool isDragging() const { return dragging; }
    // The pixels the thumb travels from -60 dB to +12 dB.
    int travel() const { return std::max (0, getHeight() - thumbHeight); }

    std::function<void (double db)> onDrag;
    std::function<void (double startDb, double endDb)> onRelease;

    void mouseDown (const juce::MouseEvent& event) override;
    void mouseDrag (const juce::MouseEvent& event) override;
    void mouseUp (const juce::MouseEvent& event) override;
    void paint (juce::Graphics& g) override;

private:
    double shownDb = 0.0;
    double dragStartDb = 0.0;
    int dragStartY = 0;
    bool dragging = false;
};

// A horizontal pan control: dragging moves pan relative to where it was grabbed (panDrag).
class PanControl final : public juce::Component
{
public:
    void setPan (double pan);
    double pan() const { return shownPan; }
    bool isDragging() const { return dragging; }

    std::function<void (double pan)> onDrag;
    std::function<void (double startPan, double endPan)> onRelease;

    void mouseDown (const juce::MouseEvent& event) override;
    void mouseDrag (const juce::MouseEvent& event) override;
    void mouseUp (const juce::MouseEvent& event) override;
    void paint (juce::Graphics& g) override;

private:
    double shownPan = 0.0;
    double dragStartPan = 0.0;
    int dragStartX = 0;
    bool dragging = false;
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
    bool enabled = true;
};

class MixerStrip final : public juce::Component
{
public:
    explicit MixerStrip (bool isMaster);

    void show (const MixerStripState& state);
    void resized() override;
    void paint (juce::Graphics& g) override;

    const song::Id& trackId() const { return current.trackId; }

    std::function<void (double db)> onVolumeDrag;
    std::function<void (double db)> onVolumeChanged;
    std::function<void (double pan)> onPanDrag;
    std::function<void (double pan)> onPanChanged;
    // A drag that ended where it started.
    std::function<void()> onGestureAbandoned;
    std::function<void (bool mute)> onMute;
    std::function<void (bool solo)> onSolo;

    juce::Label nameLabel;
    PanControl panControl;
    MixerFader fader;
    LevelMeter meter;
    juce::Label valueLabel;
    juce::TextButton muteButton { "M" };
    juce::TextButton soloButton { "S" };

private:
    void showValue();

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

    // The Song a gesture in progress would make (the mix only).
    std::function<void (const song::Song& preview)> onPreview;
    // A finished gesture: the changed Song and its undo label.
    std::function<void (song::Song changed, std::string label)> onEdit;

    std::vector<std::unique_ptr<MixerStrip>> strips;
    MixerStrip masterStrip { true };

private:
    void rebuildStrips();
    void connectStrip (MixerStrip& strip, const song::Id& trackId);
    void report (core::Expected<song::Song> changed, const std::string& label);

    song::Song current;
    bool editable = true;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Mixer)
};
} // namespace overdub::ui
