#pragma once

#include "engine/LiveInput.h"
#include "song/Song.h"

#include <juce_audio_processors_headless/juce_audio_processors_headless.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

// One Song snapshot made playable: its AudioProcessorGraph (Track nodes -> Insert Chain nodes ->
// Master -> device outputs, ADR 0001) built, prepared and warmed up off the audio thread, so the
// audio thread only ever calls render(). The engine swaps whole plans; a plan never changes after
// it is built.
namespace overdub::engine
{
// The device shape a plan is prepared for. The sample rate is the Song's.
struct EngineConfig
{
    int numOutputChannels = 2;
    int maxBlockSize = 512;
};

// Extra processors placed on a Track's path, in order, before Master. Slice 12 builds AU Insert
// Chains here; tests insert the test-only processors (ADR 0005). Returns none by default.
using TrackInsertFactory = std::function<std::vector<std::unique_ptr<juce::AudioProcessor>> (const song::Track&)>;

// Tells every Track node where the render is: time in samples = render position of the block's
// first sample, playing = transport running. Written and read on the audio thread only.
class PlanPlayHead final : public juce::AudioPlayHead
{
public:
    juce::Optional<PositionInfo> getPosition() const override;

    std::int64_t renderSample = 0;
    bool playing = false;
};

class RenderPlan
{
public:
    // Builds and prepares the graph; logs `engine.plan_built` and one warning per Clip source it
    // cannot play (`engine.clip_source_missing`, `engine.clip_source_unusable`). Never fails:
    // an unplayable source plays silence (ADR 0004).
    RenderPlan (const song::Song& song, const juce::File& packageRoot, const EngineConfig& config, const TrackInsertFactory& makeInserts);

    RenderPlan (const RenderPlan&) = delete;
    RenderPlan& operator= (const RenderPlan&) = delete;

    // Audio thread. outputs has config.numOutputChannels channels of numSamples <= maxBlockSize.
    // input is the device input of the block, which Armed Tracks hear while input.monitoring.
    // renderSample is the render position of the first sample (song position + latency).
    // Returns how many nodes rendered silence this block because another thread held their
    // callback lock (the patched graph never waits for it, ADR 0001 amendment).
    std::uint64_t render (float* const* outputs, const LiveInput& input, int numSamples, std::int64_t renderSample, bool playing) noexcept;

    int sampleRate() const { return songSampleRate; }
    int numOutputChannels() const { return config.numOutputChannels; }
    int maxBlockSize() const { return config.maxBlockSize; }
    // The graph's total latency: the longest Track path, which PDC delays every other path to.
    int latencySamples() const { return graphLatency; }
    int numTracks() const { return trackCount; }

private:
    EngineConfig config;
    int songSampleRate = 0;
    int graphLatency = 0;
    int trackCount = 0;
    PlanPlayHead playHead;
    LiveInput liveInput; // read by every Track node during render()
    juce::MidiBuffer midi; // always empty: no node reads or writes MIDI
    juce::AudioProcessorGraph graph;
};
} // namespace overdub::engine
