#pragma once

#include "engine/MixLaw.h"
#include "engine/PlainProcessor.h"

#include <atomic>

// A Mixer strip on the audio path (issue #31): the last node of each Track's path (after its
// Insert Chain) and the Master node. It multiplies its two channels by the gains in its
// StripControl and keeps the block's peak there for the meters.
//
// Threads: a non-audio thread writes the target gains and takes the peak; the audio thread reads
// the gains and raises the peak. When the gains changed since the previous block, the block ramps
// linearly from the previous gains to the new ones, so a fader move does not click.
namespace overdub::engine
{
struct StripControl
{
    std::atomic<float> leftGain { 1.0f };
    std::atomic<float> rightGain { 1.0f };
    std::atomic<float> peak { 0.0f }; // the largest |sample| since the last takePeak, post-gain

    void setGains (ChannelGains gains) noexcept;
    // The peak since the previous call; starts the next one from 0.
    float takePeak() noexcept;
    // Audio thread.
    void raisePeak (float blockPeak) noexcept;
};

class MixStripNode final : public PlainProcessor
{
public:
    // `control` must outlive the node (the plan owns both).
    MixStripNode (juce::String name, StripControl& control);

    const juce::String getName() const override { return stripName; }
    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override;

private:
    juce::String stripName;
    StripControl& control;
    ChannelGains applied; // audio thread: the gains the previous block ended on
};
} // namespace overdub::engine
