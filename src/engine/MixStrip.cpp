#include "engine/MixStrip.h"

#include <algorithm>

namespace overdub::engine
{
void StripControl::setGains (ChannelGains gains) noexcept
{
    leftGain.store (gains.left, std::memory_order_relaxed);
    rightGain.store (gains.right, std::memory_order_relaxed);
}

float PeakHold::take() noexcept
{
    return peak.exchange (0.0f, std::memory_order_relaxed);
}

void PeakHold::raise (float blockPeak) noexcept
{
    auto current = peak.load (std::memory_order_relaxed);
    while (blockPeak > current && ! peak.compare_exchange_weak (current, blockPeak, std::memory_order_relaxed))
    {
    }
}

MixStripNode::MixStripNode (juce::String name, StripControl& stripControl)
    : PlainProcessor (stereoInOut()),
      stripName (std::move (name)),
      control (stripControl),
      applied { stripControl.leftGain.load(), stripControl.rightGain.load() }
{
}

void MixStripNode::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    const ChannelGains target { control.leftGain.load (std::memory_order_relaxed), control.rightGain.load (std::memory_order_relaxed) };
    const auto numSamples = buffer.getNumSamples();
    const float from[2] { applied.left, applied.right };
    const float to[2] { target.left, target.right };

    float blockPeak = 0.0f;
    for (int channel = 0; channel < std::min (buffer.getNumChannels(), 2); ++channel)
    {
        if (juce::exactlyEqual (from[channel], to[channel]))
            buffer.applyGain (channel, 0, numSamples, to[channel]);
        else
            buffer.applyGainRamp (channel, 0, numSamples, from[channel], to[channel]);
        blockPeak = std::max (blockPeak, buffer.getMagnitude (channel, 0, numSamples));
    }
    applied = target;
    control.peak.raise (blockPeak);
}
} // namespace overdub::engine
