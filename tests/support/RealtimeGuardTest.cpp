#include "support/RealtimeGuard.h"

#include <catch2/catch_test_macros.hpp>

#include <juce_audio_basics/juce_audio_basics.h>
#include <os/lock.h>

#include <atomic>
#include <cstdlib>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

using overdub::testing::keepAllocation;
using overdub::testing::runRealtimeGuarded;

TEST_CASE ("the realtime guard counts a heap allocation made inside the guarded scope", "[support][rtguard]")
{
    const auto violations = runRealtimeGuarded ([] {
        auto* value = keepAllocation (new int (7));
        delete value;
    });

    CHECK (violations.allocations == 1);
    CHECK (violations.deallocations == 1);
    CHECK (violations.locks == 0);
}

TEST_CASE ("the realtime guard counts a std::mutex lock and an os_unfair_lock taken inside the guarded scope", "[support][rtguard]")
{
    std::mutex mutex;
    os_unfair_lock unfairLock = OS_UNFAIR_LOCK_INIT;

    const auto violations = runRealtimeGuarded ([&] {
        mutex.lock();
        mutex.unlock();
        os_unfair_lock_lock (&unfairLock);
        os_unfair_lock_unlock (&unfairLock);
    });

    CHECK (violations.locks == 2);
    CHECK (violations.allocations == 0);
    CHECK (std::string_view (violations.firstCall) == "pthread_mutex_lock");
}

TEST_CASE ("the realtime guard counts allocations and locks made inside JUCE", "[support][rtguard]")
{
    juce::CriticalSection criticalSection;
    juce::AudioBuffer<float> buffer;

    const auto violations = runRealtimeGuarded ([&] {
        const juce::ScopedLock lock (criticalSection);
        buffer.setSize (2, 512); // juce::HeapBlock -> std::malloc
    });

    CHECK (violations.locks == 1);
    CHECK (violations.allocations >= 1);
}

TEST_CASE ("the realtime guard counts C allocation functions called directly", "[support][rtguard]")
{
    const auto violations = runRealtimeGuarded ([] {
        void* block = keepAllocation (std::calloc (4, 16));
        block = keepAllocation (std::realloc (block, 128));
        std::free (block);
        void* aligned = nullptr;
        if (posix_memalign (&aligned, 64, 256) == 0)
            std::free (keepAllocation (aligned));
        std::free (nullptr);
    });

    CHECK (violations.allocations == 3);
    CHECK (violations.deallocations == 2);
    CHECK (std::string_view (violations.firstCall) == "calloc");
}

TEST_CASE ("the realtime guard records nothing for arithmetic on preallocated memory", "[support][rtguard]")
{
    std::vector<float> samples (512, 0.5f);

    const auto violations = runRealtimeGuarded ([&] {
        for (auto& sample : samples)
            sample *= 0.25f;
    });

    CHECK (violations.allocations == 0);
    CHECK (violations.deallocations == 0);
    CHECK (violations.locks == 0);
    CHECK (violations.firstCall == nullptr);
}

TEST_CASE ("the realtime guard ignores allocations outside its scope and on other threads", "[support][rtguard]")
{
    std::atomic<bool> startAllocating { false };
    std::atomic<bool> stop { false };
    std::atomic<int> otherThreadIterations { 0 };
    std::thread otherThread ([&] {
        while (! startAllocating.load())
            std::this_thread::yield();
        while (! stop.load())
        {
            std::vector<int> churn (64);
            std::mutex mutex;
            const std::lock_guard lock (mutex);
            ++otherThreadIterations;
        }
    });

    std::vector<int> before (64);
    const auto violations = runRealtimeGuarded ([&] {
        startAllocating.store (true);
        while (otherThreadIterations.load() < 1000)
            std::this_thread::yield();
    });
    std::vector<int> after (64);

    stop.store (true);
    otherThread.join();

    CHECK (violations.allocations == 0);
    CHECK (violations.locks == 0);
}

TEST_CASE ("a nested realtime guard records into its own record and restores the outer one", "[support][rtguard]")
{
    overdub::testing::RealtimeViolations inner;

    const auto outer = runRealtimeGuarded ([&] {
        inner = runRealtimeGuarded ([] { delete keepAllocation (new int (1)); });
        delete keepAllocation (new int (2));
        delete keepAllocation (new int (3));
    });

    CHECK (inner.allocations == 1);
    CHECK (outer.allocations == 2);
}
