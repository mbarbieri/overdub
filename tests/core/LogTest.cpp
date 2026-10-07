#include "core/Log.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <regex>

using overdub::core::Level;
using overdub::core::ScopedLogCapture;

TEST_CASE ("each log line is one flat JSON object with the reserved keys and the event's fields", "[core][log]")
{
    ScopedLogCapture capture;

    overdub::core::logEvent (Level::info, "app.test_event", { { "count", 3 }, { "name", "kick" }, { "ids", { 1, 2 } } }, "hello");

    const auto lines = capture.lines();
    REQUIRE (lines.size() == 1);
    CHECK (lines[0].find ('\n') == std::string::npos);

    const auto line = nlohmann::json::parse (lines[0]);
    REQUIRE (line.is_object());
    CHECK (line["event"] == "app.test_event");
    CHECK (line["lvl"] == "info");
    CHECK (line["thread"] == "worker");
    CHECK (line["msg"] == "hello");
    CHECK (line["host_ns"].is_number_unsigned());
    CHECK (line["host_ns"].get<std::uint64_t>() > 0);
    CHECK (std::regex_match (line["t"].get<std::string>(), std::regex (R"(\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}Z)")));
    CHECK (line["count"] == 3);
    CHECK (line["name"] == "kick");
    CHECK (line["ids"] == nlohmann::json ({ 1, 2 }));
}

TEST_CASE ("a nested object field is written as its JSON text so lines stay flat", "[core][log]")
{
    ScopedLogCapture capture;

    overdub::core::logEvent (Level::info, "app.test_event", { { "device", { { "rate", 48000 } } } });

    const auto line = nlohmann::json::parse (capture.lines().at (0));
    CHECK (line["device"] == R"({"rate":48000})");
}

TEST_CASE ("a field cannot overwrite a reserved key", "[core][log]")
{
    ScopedLogCapture capture;

    overdub::core::logEvent (Level::info, "app.test_event", { { "event", "spoofed" }, { "lvl", "error" } });

    const auto event = capture.events().at (0);
    CHECK (event["event"] == "app.test_event");
    CHECK (event["lvl"] == "info");
    CHECK (event["field_event"] == "spoofed");
    CHECK (event["field_lvl"] == "error");
}

TEST_CASE ("events below the minimum level are not written", "[core][log]")
{
    ScopedLogCapture capture { Level::info };

    overdub::core::logEvent (Level::debug, "app.too_quiet");
    overdub::core::logEvent (Level::warn, "app.loud_enough");

    const auto events = capture.events();
    REQUIRE (events.size() == 1);
    CHECK (events[0]["event"] == "app.loud_enough");
    CHECK (events[0]["lvl"] == "warn");
}

TEST_CASE ("the current thread's role appears in each line", "[core][log]")
{
    ScopedLogCapture capture;
    overdub::core::ScopedThreadRole role { overdub::core::ThreadRole::ui };

    overdub::core::logEvent (Level::info, "app.on_ui");

    CHECK (capture.events().at (0)["thread"] == "ui");
}

TEST_CASE ("OVERDUB_LOG names a level that overrides the build default", "[core][log]")
{
    using overdub::core::levelFromOverride;

    CHECK (levelFromOverride (nullptr, Level::info) == Level::info);
    CHECK (levelFromOverride ("", Level::info) == Level::info);
    CHECK (levelFromOverride ("debug", Level::info) == Level::debug);
    CHECK (levelFromOverride ("error", Level::debug) == Level::error);
    CHECK (levelFromOverride ("loud", Level::warn) == Level::warn);
}

TEST_CASE ("JUCE's own log messages become log events once the bridge is installed", "[core][log]")
{
    ScopedLogCapture capture;
    overdub::core::JuceLoggerBridge bridge;
    juce::Logger::setCurrentLogger (&bridge);

    juce::Logger::writeToLog ("from juce");
    juce::Logger::setCurrentLogger (nullptr);

    const auto events = capture.events();
    REQUIRE (events.size() == 1);
    CHECK (events[0]["event"] == "app.juce_message");
    CHECK (events[0]["lvl"] == "info");
    CHECK (events[0]["msg"] == "from juce");
}
