#pragma once

#include "engine/BandFilter.h"
#include "engine/MixLaw.h"
#include "engine/PlainProcessor.h"

#include <atomic>

// A Mixer strip on the audio path (issue #31): the last node of each Track's path (after its
// Insert Chain) and the Master node. It runs its two channels through its StripControl's band
// filter (engine/BandFilter.h, issue #234; only a Track strip's is ever on), multiplies them by the
// gains there and keeps the block's peak there for the meters.
//
// Threads: a non-audio thread writes the target gains and takes the peak; the audio thread reads
// the gains and raises the peak. When the gains changed since the previous block, the block ramps
// linearly from the previous gains to the new ones, so a fader move does not click.
namespace overdub::engine
{
// The largest |sample| since the last take: the audio thread raises it, a non-audio thread takes
// it (a meter's poll), so each poll covers only the blocks since the previous one.
struct PeakHold
{
    std::atomic<float> peak { 0.0f };

    // The peak since the previous call; starts the next one from 0.
    float take() noexcept;
    // Audio thread.
    void raise (float blockPeak) noexcept;
};

struct StripControl
{
    std::atomic<float> leftGain { 1.0f };
    std::atomic<float> rightGain { 1.0f };
    PeakHold peak; // post-gain
    BandFilterControl bandFilter; // pre-gain; off unless the plan's heard band filter is this Track's

    void setGains (ChannelGains gains) noexcept;
};

class MixStripNode final : public PlainProcessor
{
public:
    // `control` must outlive the node (the plan owns both).
    MixStripNode (juce::String name, StripControl& control);

    const juce::String getName() const override { return stripName; }
    void prepareToPlay (double sampleRate, int) override { preparedRate = sampleRate; }
    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override;

private:
    juce::String stripName;
    StripControl& control;
    ChannelGains applied; // audio thread: the gains the previous block ended on
    BandFilterProcessor filter; // audio thread
    double preparedRate = 0.0;
};
} // namespace overdub::engine
