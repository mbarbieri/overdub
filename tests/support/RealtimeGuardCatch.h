#pragma once

// Catch2 reporting for the test-only realtime guard (ADR 0005). Usage, typically around an
// audio callback or process() call:
//
//     REQUIRE_REALTIME_SAFE (callback.process (buffer, numSamples));
//
// The arguments run once, on this thread, inside a ScopedRealtimeGuard. The check fails
// when they allocated, freed or blocked on a lock, and the failure prints the counts and
// the first violating function. To find the offending call site, run the test under lldb
// with a breakpoint on the function named as "first call".

#include "support/RealtimeGuard.h"

#include <catch2/catch_tostring.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <string>

template <>
struct Catch::StringMaker<overdub::testing::RealtimeViolations>
{
    static std::string convert (const overdub::testing::RealtimeViolations& violations)
    {
        return "{ allocations: " + std::to_string (violations.allocations)
             + ", deallocations: " + std::to_string (violations.deallocations)
             + ", locks: " + std::to_string (violations.locks)
             + ", first call: " + (violations.firstCall != nullptr ? violations.firstCall : "none") + " }";
    }
};

namespace overdub::testing
{
class IsRealtimeSafeMatcher final : public Catch::Matchers::MatcherBase<RealtimeViolations>
{
public:
    bool match (const RealtimeViolations& violations) const override
    {
        return violations.allocations == 0 && violations.deallocations == 0 && violations.locks == 0;
    }

    std::string describe() const override
    {
        return "is realtime safe (no heap allocation, deallocation or blocking lock in the guarded scope)";
    }
};

inline IsRealtimeSafeMatcher isRealtimeSafe()
{
    return {};
}
} // namespace overdub::testing

// Variadic so arguments may contain commas, e.g. CHECK_REALTIME_SAFE (fn (a, b)).
#define CHECK_REALTIME_SAFE(...) \
    CHECK_THAT (::overdub::testing::runRealtimeGuarded ([&] { __VA_ARGS__; }), ::overdub::testing::isRealtimeSafe())

#define REQUIRE_REALTIME_SAFE(...) \
    REQUIRE_THAT (::overdub::testing::runRealtimeGuarded ([&] { __VA_ARGS__; }), ::overdub::testing::isRealtimeSafe())
