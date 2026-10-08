#pragma once

#include "engine/ClipSchedule.h"
#include "engine/LiveInput.h"
#include "engine/PlainProcessor.h"

#include <vector>

// A Track's source node in the graph: plays its Clip schedule at the position the graph's play
// head reports (time in samples = render position), and silence while the transport is stopped.
// An Armed Track also adds its live input channels while Monitoring is on (plain Monitoring,
// issue #26), transport running or not: a mono input feeds both channels, a stereo input L and R.
namespace overdub::engine
{
class TrackPlayer final : public PlainProcessor
{
public:
    // `sources` keeps every buffer the segments point into alive for the player's lifetime.
    // `liveInput` (owned by the plan) is read on every block; `monitorChannels` are the device
    // input channels this Track hears while Monitoring (empty = none: not Armed).
    TrackPlayer (juce::String trackId, std::vector<ClipSegment> segments, std::vector<SourceAudio> sources, const LiveInput& liveInput, std::vector<int> monitorChannels);

    const juce::String getName() const override { return "Track " + trackId; }
    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override;

private:
    juce::String trackId;
    std::vector<ClipSegment> segments;
    std::vector<SourceAudio> sources;
    const LiveInput& liveInput;
    std::vector<int> monitorChannels;

    void addMonitoredInput (juce::AudioBuffer<float>& buffer) const noexcept;
};
} // namespace overdub::engine
