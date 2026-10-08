#pragma once

#include "engine/PlainProcessor.h"

#include <atomic>
#include <memory>

// A graph node for an Insert Chain plugin that lives longer than one render plan (issue #33): the
// plugin host (plugin::InsertHost) owns and prepares the processor, and every plan built while it
// is loaded puts it on its path through one of these nodes. Rebuilding a plan (every Song edit)
// so never re-instantiates a plugin or loses its state.
//
// Invariants:
// - The node reports the processor's latency and tail as they were when the node was made; a
//   later latency change needs a new plan (the host reports it, the open Song reloads).
// - The node never prepares, resets or releases the processor: the host does, while no plan
//   renders it.
// - It forwards processBlock only after startForwarding(), which the plan calls once its warm-up
//   is done: until then the previous plan may be rendering the same processor on the audio thread,
//   so the warm-up passes audio through untouched.
// - Only the plan the audio thread renders forwards blocks, so the processor is never run by two
//   threads at once.
// - Plans hold the processor through a shared_ptr, so the last owner may be a retired plan. The
//   engine frees plans only off the audio thread (loadSong, housekeeping: the message thread in
//   the app), which is where a plugin may then be destroyed.
namespace overdub::engine
{
class SharedInsertNode final : public PlainProcessor
{
public:
    explicit SharedInsertNode (std::shared_ptr<juce::AudioProcessor> processor);

    const juce::String getName() const override { return name; }
    double getTailLengthSeconds() const override { return tailSeconds; }
    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;

    // Non-audio thread, after the plan's warm-up.
    void startForwarding() { forwarding.store (true, std::memory_order_release); }

private:
    std::shared_ptr<juce::AudioProcessor> shared;
    juce::String name;
    double tailSeconds = 0.0;
    std::atomic<bool> forwarding { false };
};
} // namespace overdub::engine
