#pragma once

#include "engine/RenderPlan.h"
#include "song/Song.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <memory>

// The offline render of a Song's mix (issue #37): the same RenderPlan the engine plays (Track
// nodes -> Insert Chains -> Mixer strips -> Master chain -> Master strip), driven block by block
// as fast as the caller pulls, with no device.
//
// The Song span is [0, end of the last Clip) plus the plan's tail (Track and Master Insert Chain
// tails), so effects ring out. Unlike live playback, which never hears Song samples 0..L-1 after
// a start (L = the plan's PDC latency, ADR 0003 amendment), the render feeds the graph from Song
// sample 0 and drops its first L outputs: output sample n is Song sample n for every n.
//
// One thread at a time (it may be a worker thread). The plan is built by the constructor on the
// calling thread. Invariant (the caller's): while it renders, no other plan renders the same
// shared Insert Chain plugins (engine/SharedInsertNode.h).
namespace overdub::exporting
{
// The Song sample after its last Clip ends; 0 without Clips.
song::SampleCount lastClipEnd (const song::Song& song);

class MixRender
{
public:
    // maxBlockSize: the block size the plugins were prepared for. The render is stereo.
    MixRender (const song::Song& song, const juce::File& packageRoot, int maxBlockSize, const engine::InsertFactory& makeInserts);

    MixRender (const MixRender&) = delete;
    MixRender& operator= (const MixRender&) = delete;

    static constexpr int numChannels = 2;

    int sampleRate() const { return plan->sampleRate(); }
    int maxBlockSize() const { return plan->maxBlockSize(); }
    int latencySamples() const { return plan->latencySamples(); }
    int tailSamples() const { return plan->tailSamples(); }
    song::SampleCount songEnd() const { return songEndSample; }
    // songEnd() + tailSamples(): every sample the render produces.
    song::SampleCount totalSamples() const { return songEndSample + plan->tailSamples(); }
    song::SampleCount renderedSamples() const { return produced; }

    // Renders the next Song samples, at most numSamples (<= maxBlockSize()), into `outputs`
    // (numChannels pointers). Returns how many it wrote: 0 once totalSamples() are done.
    int renderNext (float* const* outputs, int numSamples);

private:
    // Runs the graph over its first L outputs, which hold no Song audio yet.
    void skipLatency();

    std::unique_ptr<engine::RenderPlan> plan;
    song::SampleCount songEndSample = 0;
    juce::AudioBuffer<float> scratch;
    song::SampleCount graphPosition = 0; // the Song sample the Track nodes read next
    song::SampleCount produced = 0;
};
} // namespace overdub::exporting
