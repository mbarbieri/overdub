#include "control/EventTap.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <memory>

using overdub::control::EventTap;
using overdub::core::Level;

TEST_CASE ("the event tap numbers lines from 1 and hands them over once", "[control][tap]")
{
    EventTap tap;
    tap.writeLine (R"({"event":"ui.a"})");
    tap.writeLine (R"({"event":"ui.b"})");

    const auto batch = tap.drain();
    REQUIRE (batch.lines.size() == 2);
    CHECK (batch.lines[0].seq == 1);
    CHECK (batch.lines[0].line == R"({"event":"ui.a"})");
    CHECK (batch.lines[1].seq == 2);
    CHECK (batch.dropped == 0);

    tap.writeLine (R"({"event":"ui.c"})");
    const auto next = tap.drain();
    REQUIRE (next.lines.size() == 1);
    CHECK (next.lines[0].seq == 3);
}

TEST_CASE ("a full event tap drops its oldest lines and counts them", "[control][tap]")
{
    EventTap tap { 3 };
    for (int i = 0; i < 5; ++i)
        tap.writeLine ("line " + std::to_string (i));

    const auto batch = tap.drain();
    REQUIRE (batch.lines.size() == 3);
    CHECK (batch.lines[0].seq == 3);
    CHECK (batch.lines[0].line == "line 2");
    CHECK (batch.lines[2].seq == 5);
    CHECK (batch.dropped == 2);
    CHECK (tap.drain().dropped == 0);
}

TEST_CASE ("a logger tap gets every written line after the sink, without the sink being replaced", "[control][tap]")
{
    overdub::core::Logger logger;
    logger.setMinLevel (Level::debug);
    auto sink = std::make_shared<overdub::core::MemoryLogSink>();
    auto tap = std::make_shared<EventTap>();
    logger.setSink (sink);
    logger.addTap (tap);

    const overdub::core::Fields fields { { "n", 1 } };
    logger.write ({ .level = Level::info, .event = "ui.tapped", .fields = &fields });
    logger.setSink (nullptr);
    logger.write ({ .level = Level::info, .event = "ui.tapped_without_sink" });
    logger.removeTap (tap);
    logger.write ({ .level = Level::info, .event = "ui.after_remove" });

    REQUIRE (sink->lines().size() == 1);
    const auto batch = tap->drain();
    REQUIRE (batch.lines.size() == 2);
    CHECK (batch.lines[0].line == sink->lines()[0]);
    CHECK (nlohmann::json::parse (batch.lines[0].line)["n"] == 1);
    CHECK (nlohmann::json::parse (batch.lines[1].line)["event"] == "ui.tapped_without_sink");
}

TEST_CASE ("ScopedLogCapture still captures while a tap is installed on the process logger", "[control][tap]")
{
    auto tap = std::make_shared<EventTap>();
    overdub::core::processLogger().addTap (tap);
    {
        overdub::core::ScopedLogCapture capture;
        overdub::core::logEvent (Level::info, "ui.both_see_it", { { "k", "v" } });

        const auto events = capture.events();
        REQUIRE (events.size() == 1);
        CHECK (events[0]["event"] == "ui.both_see_it");
        CHECK (events[0]["k"] == "v");
    }
    overdub::core::processLogger().removeTap (tap);

    const auto batch = tap->drain();
    REQUIRE (batch.lines.size() == 1);
    CHECK (nlohmann::json::parse (batch.lines[0].line)["event"] == "ui.both_see_it");
}
