#pragma once

#include "engine/LiveInput.h"
#include "engine/MixStrip.h"
#include "song/PracticeSpeed.h"
#include "song/Song.h"

#include <juce_audio_processors_headless/juce_audio_processors_headless.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>

// One Song snapshot made playable: its AudioProcessorGraph (Track nodes -> Insert Chain nodes ->
// Track Mixer strip -> Master Insert Chain -> Master strip -> device outputs, ADR 0001, issues
// #31, #33) built, prepared and
// warmed up off the audio thread, so the audio thread only ever calls render(). The engine swaps
// whole plans; after it is built, a plan's graph never changes: only its strips' gains do
// (applyMix, through atomics).
namespace overdub::engine
{
// The device shape a plan is prepared for. The sample rate is the Song's.
struct EngineConfig
{
    int numOutputChannels = 2;
    int maxBlockSize = 512;
};

// The processors of one Insert Chain, in order: a Track's (trackId set) or the Master's (trackId
// empty), from that chain's slots. Test-only processors (ADR 0005) are made per plan, and the
// graph prepares them; plugins are shared across plans as engine::SharedInsertNodes made by
// plugin::InsertHost (issue #33). Returns none by default.
using InsertFactory = std::function<std::vector<std::unique_ptr<juce::AudioProcessor>> (const song::Id& trackId, const std::vector<song::InsertSlot>& chain)>;

// The Armed Tracks that hear their input while Monitoring is on (issue #26). Monitoring is per
// Track and transient, not in the Song. nullopt: every Armed Track.
using MonitoredTracks = std::optional<std::set<song::Id>>;

// How a plan plays the Song at a Practice Speed (ADR 0009, issue #212), a whole percent. At 100
// every source plays its own file (song::playedFileFor) and `renders` is ignored. At any other
// speed the plan runs in plan time, Song time × 100 / speedPercent: each Clip source plays its
// render listed here (a float WAV at the Song's rate, about length × 100 / speedPercent long), and a
// source without one plays silence. Clips, fades and gains keep their Song times.
struct PracticePlayback
{
    int speedPercent = song::normalPracticeSpeed;
    std::map<song::ClipSource, juce::File> renders;
};

// The plan sample that plays Song sample `songSample` at `speedPercent`, and back (rounded to the
// nearest). Identities at 100.
std::int64_t songToPlanSample (std::int64_t songSample, int speedPercent);
std::int64_t planToSongSample (std::int64_t planSample, int speedPercent);

// Tells every Track node where the render is: time in samples = render position of the block's
// first sample, playing = transport running. Written and read on the audio thread only.
class PlanPlayHead final : public juce::AudioPlayHead
{
public:
    juce::Optional<PositionInfo> getPosition() const override;

    std::int64_t renderSample = 0;
    bool playing = false;
};

// The peaks since the previous takeMeterPeaks (linear), by Track id: each strip's post-fader peak,
// and each Armed Track's input peak (issue #131): the raw device input on the channels it records,
// before its Insert Chain and fader, whether or not it is monitored or a Pass runs. A Track that is
// not Armed has no input peak.
struct MeterPeaks
{
    std::map<song::Id, float> tracks;
    std::map<song::Id, float> inputs;
    float master = 0.0f;
};

class RenderPlan
{
public:
    // Builds and prepares the graph; logs `engine.plan_built` and one warning per Clip source it
    // cannot play (`engine.clip_source_missing`, `engine.clip_source_unusable`). Never fails:
    // an unplayable source plays silence (ADR 0004), and so does one without a render at a
    // Practice Speed (not logged: it is expected while renders are made).
    RenderPlan (const song::Song& song, const juce::File& packageRoot, const EngineConfig& config, const InsertFactory& makeInserts, const MonitoredTracks& monitoredTracks = std::nullopt, const PracticePlayback& practice = {});

    RenderPlan (const RenderPlan&) = delete;
    RenderPlan& operator= (const RenderPlan&) = delete;

    // Audio thread. outputs has config.numOutputChannels channels of numSamples <= maxBlockSize.
    // input is the device input of the block, which the monitored Armed Tracks hear while
    // input.monitoring, and whose peaks every Armed Track's input meter takes.
    // renderSample is the render position of the first sample (plan position + latency).
    // Returns how many nodes rendered silence this block because another thread held their
    // callback lock (the patched graph never waits for it, ADR 0001 amendment).
    std::uint64_t render (float* const* outputs, const LiveInput& input, int numSamples, std::int64_t renderSample, bool playing) noexcept;

    int sampleRate() const { return songSampleRate; }
    // The Practice Speed it plays at: render() takes plan samples (see PracticePlayback).
    int speedPercent() const { return practiceSpeedPercent; }
    int numOutputChannels() const { return config.numOutputChannels; }
    int maxBlockSize() const { return config.maxBlockSize; }
    // The graph's total latency: the longest Track path, which PDC delays every other path to.
    // Includes every Insert Chain plugin's reported latency (PDC, issue #33).
    int latencySamples() const { return graphLatency; }
    // The longest tail any path adds after its input falls silent: the sum of its Track's and the
    // Master's Insert Chain tails (AudioProcessor::getTailLengthSeconds), in samples. The engine
    // keeps rendering while stopped, so tails ring out at stop; export renders this much past the
    // Song's end.
    int tailSamples() const { return graphTail; }
    int numTracks() const { return trackCount; }

    // Non-audio thread. Whether `song` has exactly this plan's Tracks (ids, in order), so its mix
    // can be applied here with applyMix.
    bool hasTracksOf (const song::Song& song) const;
    // Non-audio thread. Sets every strip's gains from the Song's mix (engine/MixLaw.h); the audio
    // thread hears them from its next block. The Song must pass hasTracksOf.
    void applyMix (const song::Song& song);
    // Non-audio thread. The peaks since the previous call; each starts again from 0.
    MeterPeaks takeMeterPeaks();

private:
    // An Armed Track's input meter: the device input channels it records.
    struct InputTap
    {
        song::Id trackId;
        std::vector<int> channels;
        PeakHold peak;
    };

    void tapInputs (const LiveInput& input, int numSamples) noexcept;

    EngineConfig config;
    int songSampleRate = 0;
    int practiceSpeedPercent = song::normalPracticeSpeed;
    int graphLatency = 0;
    int graphTail = 0;
    int trackCount = 0;
    std::vector<song::Id> trackIds;
    // The strips' controls, read by the strip nodes: declared before the graph, so they outlive it.
    std::vector<std::unique_ptr<StripControl>> trackStrips;
    StripControl masterStrip;
    std::vector<std::unique_ptr<InputTap>> inputTaps; // one per Armed Track, in Song order
    PlanPlayHead playHead;
    LiveInput liveInput; // read by every Track node during render()
    juce::MidiBuffer midi; // always empty: no node reads or writes MIDI
    juce::AudioProcessorGraph graph;
};
} // namespace overdub::engine
