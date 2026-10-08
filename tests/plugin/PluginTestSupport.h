#pragma once

#include "core/Error.h"
#include "plugin/AudioUnitLoader.h"
#include "song/Song.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <catch2/catch_test_macros.hpp>

#include <CoreFoundation/CoreFoundation.h>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>

// Helpers for plugin tests (issue #33). Out-of-process AUs arrive asynchronously, through the main
// run loop (JUCE's message queue and the AU completion), so tests run it until a condition holds.
// The test thread is the main thread and, under ScopedJuceInitialiser_GUI, the message thread.
namespace overdub::plugin::testing
{
// Runs the main run loop until `done` or the timeout; returns `done()`.
inline bool runMessagesUntil (const std::function<bool()>& done, std::chrono::milliseconds timeout = std::chrono::seconds (30))
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (! done() && std::chrono::steady_clock::now() < deadline)
        CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.01, true);
    return done();
}

// Runs the main run loop for a while, so anything already posted is delivered.
inline void runMessagesFor (std::chrono::milliseconds duration)
{
    runMessagesUntil ([] { return false; }, duration);
}

// Apple's AUs, on every Mac (ADR 0005): AUPeakLimiter reports real latency; AUDelay has state.
inline song::InsertSlot peakLimiterSlot (std::string id = "slot-limiter")
{
    return song::InsertSlot { .id = std::move (id), .component = { "aufx", "lmtr", "appl" }, .name = "AUPeakLimiter" };
}

inline song::InsertSlot delaySlot (std::string id = "slot-delay")
{
    return song::InsertSlot { .id = std::move (id), .component = { "aufx", "dely", "appl" }, .name = "AUDelay" };
}

// Instantiates the slot through the loader and waits for it.
inline core::Expected<std::unique_ptr<juce::AudioProcessor>> instantiateAndWait (AudioUnitLoader& loader, const song::InsertSlot& slot, double sampleRate, int blockSize)
{
    std::optional<core::Expected<std::unique_ptr<juce::AudioProcessor>>> result;
    loader.instantiate (slot, sampleRate, blockSize, [&result] (core::Expected<std::unique_ptr<juce::AudioProcessor>> created) { result = std::move (created); });
    REQUIRE (runMessagesUntil ([&result] { return result.has_value(); }));
    return std::move (*result);
}
} // namespace overdub::plugin::testing
