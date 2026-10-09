#include "engine/SharedInsertNode.h"

namespace overdub::engine
{
SharedInsertNode::SharedInsertNode (std::shared_ptr<juce::AudioProcessor> processor, std::shared_ptr<const std::atomic<bool>> crashedFlag)
    : PlainProcessor (stereoInOut()),
      shared (std::move (processor)),
      crashed (std::move (crashedFlag)),
      name (shared->getName()),
      tailSeconds (shared->getTailLengthSeconds())
{
    // Off the audio thread: setLatencySamples notifies listeners.
    const auto latency = shared->getLatencySamples();
    setLatencySamples (latency);
    if (crashed != nullptr && latency > 0)
    {
        delayLine.setSize (2, latency);
        delayLine.clear();
    }
}

void SharedInsertNode::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    if (! forwarding.load (std::memory_order_acquire))
        return;
    if (crashed != nullptr && crashed->load (std::memory_order_acquire))
    {
        passThroughDelayed (buffer);
        return;
    }
    shared->processBlock (buffer, midi);
}

void SharedInsertNode::passThroughDelayed (juce::AudioBuffer<float>& buffer)
{
    const auto length = delayLine.getNumSamples();
    if (length == 0)
        return;
    const auto channels = std::min (buffer.getNumChannels(), delayLine.getNumChannels());
    auto index = delayIndex;
    for (int channel = 0; channel < channels; ++channel)
    {
        auto* samples = buffer.getWritePointer (channel);
        auto* line = delayLine.getWritePointer (channel);
        index = delayIndex;
        for (int i = 0; i < buffer.getNumSamples(); ++i)
        {
            std::swap (samples[i], line[index]);
            index = (index + 1) % length;
        }
    }
    delayIndex = index;
}
} // namespace overdub::engine
