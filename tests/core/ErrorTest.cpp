#include "core/Error.h"
#include "core/Log.h"

#include <catch2/catch_test_macros.hpp>

using overdub::core::Error;
using overdub::core::Expected;
using overdub::core::Level;
using overdub::core::ScopedLogCapture;

namespace
{
Expected<int> parsePositive (int value)
{
    if (value <= 0)
        return std::unexpected (Error { "song.not_positive", "value must be positive", { { "value", value } } });

    return value;
}
} // namespace

TEST_CASE ("an Error turns into one log event named by its code", "[core][error]")
{
    ScopedLogCapture capture;

    const auto result = parsePositive (-4);
    REQUIRE_FALSE (result.has_value());
    overdub::core::logError (result.error());

    const auto events = capture.events();
    REQUIRE (events.size() == 1);
    CHECK (events[0]["event"] == "song.not_positive");
    CHECK (events[0]["lvl"] == "error");
    CHECK (events[0]["value"] == -4);
}

TEST_CASE ("an Error can be logged at a lower level when the caller recovered", "[core][error]")
{
    ScopedLogCapture capture;

    overdub::core::logError (Error { "plugin.scan_skipped", "skipped", {} }, Level::warn);

    CHECK (capture.events().at (0)["lvl"] == "warn");
}

TEST_CASE ("a passing OVERDUB_CHECK logs nothing and yields true", "[core][error]")
{
    ScopedLogCapture capture;

    const bool held = OVERDUB_CHECK (1 + 1 == 2, "app.arithmetic");

    CHECK (held);
    CHECK (capture.lines().empty());
}

// In debug builds the failed check also hits jassertfalse, which only stops under a debugger.
TEST_CASE ("a failing OVERDUB_CHECK logs one error with its fields and file:line and yields false", "[core][error]")
{
    ScopedLogCapture capture;
    const int frames = 7;

    const bool held = OVERDUB_CHECK (frames == 8, "video.frame_count", { "frames", frames });

    CHECK_FALSE (held);
    const auto events = capture.events();
    REQUIRE (events.size() == 1);
    CHECK (events[0]["event"] == "video.frame_count");
    CHECK (events[0]["lvl"] == "error");
    CHECK (events[0]["frames"] == 7);
    CHECK (events[0]["condition"] == "frames == 8");
    CHECK (events[0]["where"].get<std::string>().find ("ErrorTest.cpp:") != std::string::npos);
}

TEST_CASE ("a failing OVERDUB_CHECK keeps caller fields named condition or where as field_<key>", "[core][error]")
{
    ScopedLogCapture capture;

    OVERDUB_CHECK (false, "song.clash", { "condition", "mine" }, { "where", "here" });

    const auto events = capture.events();
    REQUIRE (events.size() == 1);
    CHECK (events[0]["condition"] == "false");
    CHECK (events[0]["field_condition"] == "mine");
    CHECK (events[0]["field_where"] == "here");
    CHECK (events[0]["where"].get<std::string>().find ("ErrorTest.cpp:") != std::string::npos);
}
