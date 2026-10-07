#include "core/Log.h"
#include "core/RtEventRing.h"
#include "support/RealtimeGuardCatch.h"

#include <catch2/catch_test_macros.hpp>

#include <array>

using overdub::core::Level;
using overdub::core::RtEvent;
using overdub::core::RtEventRing;
using overdub::core::RtEventSpec;
using overdub::core::ScopedLogCapture;

namespace
{
// A stand-in for a feature's RT enum and its name table.
enum TestRtCode : std::uint16_t
{
    xrun = 0,
    clipped = 1
};

constexpr std::array<RtEventSpec, 2> testCatalog { {
    { "engine.xrun", Level::warn, { "missed_samples" } },
    { "engine.clipped", Level::info, { "channel", "peak" } },
} };

RtEvent makeEvent (std::uint16_t code, std::int64_t sample, std::uint64_t hostTimeNs, double arg0 = 0.0, double arg1 = 0.0)
{
    return RtEvent { code, sample, hostTimeNs, { arg0, arg1, 0.0, 0.0 } };
}
} // namespace

TEST_CASE ("RT events drain in push order with their capture-time sample and host_ns", "[core][rt]")
{
    ScopedLogCapture capture;
    RtEventRing ring { 8 };

    REQUIRE (ring.push (makeEvent (xrun, 1024, 5'000'000'000, 64)));
    REQUIRE (ring.push (makeEvent (clipped, 2048, 5'000'100'000, 1, 1.25)));

    overdub::core::drainRtEvents (ring, testCatalog);

    const auto events = capture.events();
    REQUIRE (events.size() == 2);

    CHECK (events[0]["event"] == "engine.xrun");
    CHECK (events[0]["lvl"] == "warn");
    CHECK (events[0]["thread"] == "audio");
    CHECK (events[0]["sample"] == 1024);
    CHECK (events[0]["host_ns"] == 5'000'000'000ULL);
    CHECK (events[0]["missed_samples"] == 64.0);
    CHECK_FALSE (events[0].contains ("arg1"));

    CHECK (events[1]["event"] == "engine.clipped");
    CHECK (events[1]["sample"] == 2048);
    CHECK (events[1]["host_ns"] == 5'000'100'000ULL);
    CHECK (events[1]["channel"] == 1.0);
    CHECK (events[1]["peak"] == 1.25);
}

TEST_CASE ("pushing RT events, including into a full ring, neither allocates nor locks", "[core][rt]")
{
    RtEventRing ring { 1 };
    const auto event = makeEvent (xrun, 1, 1);

    REQUIRE_REALTIME_SAFE (ring.push (event); ring.push (event));
    CHECK (ring.dropped() == 1);
}

TEST_CASE ("a drained ring is empty until the next push", "[core][rt]")
{
    ScopedLogCapture capture;
    RtEventRing ring { 4 };
    ring.push (makeEvent (xrun, 1, 1));

    overdub::core::drainRtEvents (ring, testCatalog);
    overdub::core::drainRtEvents (ring, testCatalog);

    CHECK (capture.events().size() == 1);
}

TEST_CASE ("overflowing the ring counts dropped events and the next drain logs the count once", "[core][rt]")
{
    ScopedLogCapture capture;
    RtEventRing ring { 2 };

    CHECK (ring.push (makeEvent (xrun, 1, 1)));
    CHECK (ring.push (makeEvent (xrun, 2, 2)));
    CHECK_FALSE (ring.push (makeEvent (xrun, 3, 3)));
    CHECK_FALSE (ring.push (makeEvent (xrun, 4, 4)));
    CHECK (ring.dropped() == 2);

    overdub::core::drainRtEvents (ring, testCatalog);

    auto events = capture.events();
    REQUIRE (events.size() == 3);
    CHECK (events[0]["sample"] == 1);
    CHECK (events[1]["sample"] == 2);
    CHECK (events[2]["event"] == "engine.rt_events_dropped");
    CHECK (events[2]["lvl"] == "warn");
    CHECK (events[2]["dropped"] == 2);
    CHECK (ring.dropped() == 0);

    overdub::core::drainRtEvents (ring, testCatalog);
    CHECK (capture.events().size() == 3);
}

TEST_CASE ("an RT code missing from the catalog is logged as unknown instead of being lost", "[core][rt]")
{
    ScopedLogCapture capture;
    RtEventRing ring { 2 };
    ring.push (makeEvent (99, 10, 20));

    overdub::core::drainRtEvents (ring, testCatalog);

    const auto events = capture.events();
    REQUIRE (events.size() == 1);
    CHECK (events[0]["event"] == "engine.rt_unknown_code");
    CHECK (events[0]["code"] == 99);
    CHECK (events[0]["sample"] == 10);
}

TEST_CASE ("the drain thread logs pending RT events at the latest when it stops", "[core][rt]")
{
    ScopedLogCapture capture;
    RtEventRing ring { 8 };
    ring.push (makeEvent (xrun, 7, 70));

    {
        overdub::core::RtEventDrainThread drainer { ring, testCatalog };
    }

    const auto events = capture.events();
    REQUIRE (events.size() == 1);
    CHECK (events[0]["sample"] == 7);
}
