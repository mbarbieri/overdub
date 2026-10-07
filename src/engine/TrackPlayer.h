#pragma once

#include "engine/ClipSchedule.h"
#include "engine/PlainProcessor.h"

#include <vector>

// A Track's source node in the graph: plays its Clip schedule at the position the graph's play
// head reports (time in samples = render position), and silence while the transport is stopped.
namespace overdub::engine
{
class TrackPlayer final : public PlainProcessor
{
public:
    // `sources` keeps every buffer the segments point into alive for the player's lifetime.
    TrackPlayer (juce::String trackId, std::vector<ClipSegment> segments, std::vector<SourceAudio> sources);

    const juce::String getName() const override { return "Track " + trackId; }
    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override;

private:
    juce::String trackId;
    std::vector<ClipSegment> segments;
    std::vector<SourceAudio> sources;
};

// The Master node: every Track sums into its input (the graph does the summing). Pass-through in
// this slice; Master volume and Insert Chain arrive in later slices.
class MasterNode final : public PlainProcessor
{
public:
    MasterNode() : PlainProcessor (stereoInOut()) {}

    const juce::String getName() const override { return "Master"; }
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
};
} // namespace overdub::engine
