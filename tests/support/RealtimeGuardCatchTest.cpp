#include "support/RealtimeGuardCatch.h"

#include <catch2/catch_test_macros.hpp>

#include <mutex>

using overdub::testing::RealtimeViolations;

TEST_CASE ("realtime violations print their counts and the first violating call", "[support][rtguard]")
{
    const RealtimeViolations violations { .allocations = 2, .deallocations = 1, .locks = 3, .firstCall = "malloc" };

    CHECK (Catch::StringMaker<RealtimeViolations>::convert (violations)
           == "{ allocations: 2, deallocations: 1, locks: 3, first call: malloc }");
}

TEST_CASE ("clean realtime violations print zero counts and no first call", "[support][rtguard]")
{
    CHECK (Catch::StringMaker<RealtimeViolations>::convert (RealtimeViolations {})
           == "{ allocations: 0, deallocations: 0, locks: 0, first call: none }");
}

TEST_CASE ("the realtime-safe matcher rejects any allocation, deallocation or lock", "[support][rtguard]")
{
    const auto matcher = overdub::testing::isRealtimeSafe();

    CHECK (matcher.match (RealtimeViolations {}));
    CHECK_FALSE (matcher.match (RealtimeViolations { .allocations = 1, .firstCall = "malloc" }));
    CHECK_FALSE (matcher.match (RealtimeViolations { .deallocations = 1, .firstCall = "free" }));
    CHECK_FALSE (matcher.match (RealtimeViolations { .locks = 1, .firstCall = "pthread_mutex_lock" }));
}

// Acceptance (issue #22): the guard fails a test that allocates or locks in a guarded scope.
// [!shouldfail] inverts the result, so these pass only if CHECK_REALTIME_SAFE fails them.

TEST_CASE ("CHECK_REALTIME_SAFE fails a test that allocates inside the guarded scope", "[support][rtguard][!shouldfail]")
{
    CHECK_REALTIME_SAFE (delete new int (42));
}

TEST_CASE ("CHECK_REALTIME_SAFE fails a test that locks inside the guarded scope", "[support][rtguard][!shouldfail]")
{
    std::mutex mutex;
    CHECK_REALTIME_SAFE (mutex.lock(), mutex.unlock());
}

TEST_CASE ("CHECK_REALTIME_SAFE passes a test that neither allocates nor locks", "[support][rtguard]")
{
    int samples[64] = {};
    CHECK_REALTIME_SAFE (for (auto& sample : samples) sample += 1);
    REQUIRE_REALTIME_SAFE (samples[0] *= 2, samples[1] *= 3);

    CHECK (samples[0] == 2);
    CHECK (samples[1] == 3);
}
