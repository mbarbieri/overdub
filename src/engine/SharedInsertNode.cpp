#include "engine/SharedInsertNode.h"

namespace overdub::engine
{
SharedInsertNode::SharedInsertNode (std::shared_ptr<juce::AudioProcessor> processor)
    : PlainProcessor (stereoInOut()),
      shared (std::move (processor)),
      name (shared->getName()),
      tailSeconds (shared->getTailLengthSeconds())
{
    // Off the audio thread: setLatencySamples notifies listeners.
    setLatencySamples (shared->getLatencySamples());
}

void SharedInsertNode::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    if (forwarding.load (std::memory_order_acquire))
        shared->processBlock (buffer, midi);
}
} // namespace overdub::engine
