#include "control/Controller.h"

#include "control/ControllerHarness.h"
#include "core/BuildInfo.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <unistd.h>

#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace overdub::control;
using namespace overdub::control::testing;
using nlohmann::json;
using overdub::core::Level;

namespace
{
std::string logLine (std::string_view event, Level level = Level::info, const overdub::core::Fields& fields = {})
{
    return overdub::core::formatLogLine ({ .level = level, .event = event, .fields = &fields });
}

const Clock::time_point start {};

void checkSeqNeverGoesBackwards (const std::vector<json>& lines)
{
    for (std::size_t i = 1; i < lines.size(); ++i)
        CHECK (lines[i - 1]["seq"].get<std::uint64_t>() <= lines[i]["seq"].get<std::uint64_t>());
}
} // namespace

TEST_CASE ("hello answers the protocol version, the app version and the pid", "[control][controller]")
{
    Controller controller { std::make_shared<EventTap>() };
    const auto client = controller.connect();

    controller.receive (client, "{\"id\":1,\"op\":\"hello\"}\n", start);
    controller.pump (start);

    const auto lines = takeLines (controller, client);
    REQUIRE (lines.size() == 1);
    CHECK (lines[0] == json { { "id", 1 }, { "ok", true }, { "seq", 0 }, { "protocol", 1 }, { "app", "Overdub" }, { "version", overdub::core::currentBuildInfo().version }, { "pid", (int) ::getpid() } });
}

TEST_CASE ("a request split across reads runs once its line is complete, and two in one read run in order", "[control][controller]")
{
    Controller controller { std::make_shared<EventTap>() };
    const auto client = controller.connect();

    controller.receive (client, "{\"id\":\"a\",", start);
    controller.receive (client, "\"op\":\"hel", start);
    controller.pump (start);
    CHECK (controller.pendingOutput (client).empty());

    controller.receive (client, "lo\"}\r\n{\"id\":2,\"op\":\"hello\"}\n{\"id\":3,", start);
    controller.pump (start);

    const auto lines = takeLines (controller, client);
    REQUIRE (lines.size() == 2);
    CHECK (lines[0]["id"] == "a");
    CHECK (lines[0]["ok"] == true);
    CHECK (lines[1]["id"] == 2);
}

TEST_CASE ("malformed requests get protocol errors and the Connection stays usable", "[control][controller]")
{
    Controller controller { std::make_shared<EventTap>() };
    const auto client = controller.connect();

    controller.receive (client, "not json\n[1,2]\n{\"op\":\"hello\"}\n{\"id\":7}\n{\"id\":8,\"op\":\"subscribe\",\"level\":\"loud\"}\n{\"id\":9,\"op\":\"hello\"}\n", start);
    controller.pump (start);

    const auto lines = takeLines (controller, client);
    REQUIRE (lines.size() == 6);
    CHECK (lines[0]["id"].is_null());
    CHECK (lines[0]["ok"] == false);
    CHECK (lines[0]["error"]["code"] == "control.bad_json");
    CHECK (lines[1]["error"]["code"] == "control.bad_request");
    CHECK (lines[2]["error"]["code"] == "control.bad_request");
    CHECK (lines[2]["error"]["fields"] == json { { "key", "id" } });
    CHECK (lines[3]["id"] == 7);
    CHECK (lines[3]["error"]["fields"] == json { { "key", "op" } });
    CHECK (lines[4]["id"] == 8);
    CHECK (lines[4]["error"]["code"] == "control.bad_field");
    CHECK (lines[4]["error"]["fields"] == json { { "key", "level" } });
    CHECK (lines[5]["ok"] == true);
    CHECK (! controller.closeReason (client).has_value());
}

TEST_CASE ("an op the Controller does not know goes to the handler, which answers control.unknown_op by default", "[control][controller]")
{
    SECTION ("default")
    {
        Controller controller { std::make_shared<EventTap>() };
        const auto client = controller.connect();
        controller.receive (client, "{\"id\":1,\"op\":\"play_or_stop\"}\n", start);
        controller.pump (start);

        const auto lines = takeLines (controller, client);
        REQUIRE (lines.size() == 1);
        CHECK (lines[0]["ok"] == false);
        CHECK (lines[0]["error"]["code"] == "control.unknown_op");
        CHECK (lines[0]["error"]["fields"] == json { { "op", "play_or_stop" } });
    }
    SECTION ("injected")
    {
        std::vector<Request> seen;
        Controller controller { std::make_shared<EventTap>(), [&seen] (const Request& request) -> Reply {
            seen.push_back (request);
            return json { { "track_id", "t-1" } };
        } };
        const auto client = controller.connect();
        controller.receive (client, "{\"id\":4,\"op\":\"add_track\",\"name\":\"Bass\"}\n", start);
        controller.pump (start);

        REQUIRE (seen.size() == 1);
        CHECK (seen[0].connection == client);
        CHECK (seen[0].id == 4);
        CHECK (seen[0].op == "add_track");
        CHECK (seen[0].fields == json { { "name", "Bass" } });
        const auto lines = takeLines (controller, client);
        REQUIRE (lines.size() == 1);
        CHECK (lines[0] == json { { "id", 4 }, { "ok", true }, { "seq", 0 }, { "track_id", "t-1" } });
    }
}

TEST_CASE ("every op in Controller::ownOps is answered by the Controller and never reaches the handler", "[control][controller]")
{
    std::vector<std::string> handled;
    Controller controller { std::make_shared<EventTap>(), [&handled] (const Request& request) -> Reply {
        handled.push_back (request.op);
        return json::object();
    } };
    const auto client = controller.connect();

    for (const auto op : Controller::ownOps)
    {
        controller.receive (client, json { { "id", 1 }, { "op", op } }.dump() + "\n", start);
        controller.pump (start);
        CHECK (takeLines (controller, client).size() == 1);
    }

    CHECK (Controller::ownOps == std::array<std::string_view, 4> { "hello", "subscribe", "wait", "state" });
    CHECK (handled.empty());
}

TEST_CASE ("a handler's result cannot overwrite a reply's id, ok or seq", "[control][controller]")
{
    Controller controller { std::make_shared<EventTap>(), [] (const Request&) -> Reply {
        return json { { "id", 99 }, { "ok", false }, { "seq", 1234 }, { "track_id", "t-1" } };
    } };
    const auto client = controller.connect();
    controller.receive (client, "{\"id\":4,\"op\":\"add_track\"}\n", start);
    controller.pump (start);

    const auto lines = takeLines (controller, client);
    REQUIRE (lines.size() == 1);
    CHECK (lines[0] == json { { "id", 4 }, { "ok", true }, { "seq", 0 }, { "track_id", "t-1" } });
}

TEST_CASE ("subscribe streams the log lines matching its prefixes and level, and nothing before it", "[control][controller]")
{
    auto tap = std::make_shared<EventTap>();
    Controller controller { tap };
    const auto subscriber = controller.connect();
    const auto bystander = controller.connect();

    tap->writeLine (logLine ("ui.before_subscribe"));
    controller.pump (start);

    controller.receive (subscriber, "{\"id\":1,\"op\":\"subscribe\",\"events\":[\"ui.\",\"song.pass\"],\"level\":\"info\"}\n", start);
    tap->writeLine (logLine ("ui.track_added", Level::info, { { "track_id", "t-1" } }));
    tap->writeLine (logLine ("ui.debug_detail", Level::debug));
    tap->writeLine (logLine ("song.pass_saved", Level::warn));
    tap->writeLine (logLine ("engine.device_opened"));
    controller.pump (start);

    const auto lines = takeLines (controller, subscriber);
    REQUIRE (lines.size() == 3);
    CHECK (lines[0]["seq"] == 2);
    CHECK (lines[0]["event"]["event"] == "ui.track_added");
    CHECK (lines[0]["event"]["track_id"] == "t-1");
    CHECK (lines[1]["seq"] == 4);
    CHECK (lines[1]["event"]["event"] == "song.pass_saved");
    CHECK (lines[2]["id"] == 1);
    CHECK (lines[2]["seq"] == 5);
    CHECK (takeLines (controller, bystander).empty());
}

TEST_CASE ("a request's events reach every subscriber before its reply, and the reply's seq bounds them", "[control][controller]")
{
    overdub::core::ScopedLogCapture capture;
    ProcessTap installed { std::make_shared<EventTap>() };
    Controller controller { installed.tap, [] (const Request& request) -> Reply {
        overdub::core::logEvent (Level::info, "ui.track_added", { { "op", request.op } });
        overdub::core::logEvent (Level::info, "ui.selection_changed");
        return json::object();
    } };
    const auto requester = controller.connect();
    const auto watcher = controller.connect();
    controller.receive (requester, "{\"id\":1,\"op\":\"subscribe\",\"events\":[\"ui.\"]}\n", start);
    controller.receive (watcher, "{\"id\":1,\"op\":\"subscribe\",\"events\":[\"ui.\"]}\n", start);
    controller.pump (start);
    takeLines (controller, requester);
    takeLines (controller, watcher);

    controller.receive (requester, "{\"id\":2,\"op\":\"add_track\"}\n", start);
    CHECK (controller.pendingOutput (requester).empty());
    controller.pump (start);

    const auto lines = takeLines (controller, requester);
    REQUIRE (lines.size() == 3);
    CHECK (lines[0]["event"]["event"] == "ui.track_added");
    CHECK (lines[1]["event"]["event"] == "ui.selection_changed");
    CHECK (lines[2]["id"] == 2);
    CHECK (lines[0]["seq"].get<std::uint64_t>() < lines[1]["seq"].get<std::uint64_t>());
    CHECK (lines[1]["seq"].get<std::uint64_t>() <= lines[2]["seq"].get<std::uint64_t>());

    const auto watched = takeLines (controller, watcher);
    REQUIRE (watched.size() == 2);
    CHECK (watched[0]["seq"] == lines[0]["seq"]);
    CHECK (watched[1]["seq"] == lines[1]["seq"]);
}

TEST_CASE ("seq never goes backwards across replies and events", "[control][controller]")
{
    auto tap = std::make_shared<EventTap>();
    Controller controller { tap };
    const auto client = controller.connect();
    controller.receive (client, "{\"id\":0,\"op\":\"subscribe\"}\n", start);

    std::vector<json> lines;
    for (int i = 1; i <= 5; ++i)
    {
        tap->writeLine (logLine ("ui.step"));
        controller.receive (client, "{\"id\":" + std::to_string (i) + ",\"op\":\"hello\"}\n", start);
        if (i % 2 == 0)
            tap->writeLine (logLine ("ui.step"));
        controller.pump (start);
        for (auto& line : takeLines (controller, client))
            lines.push_back (std::move (line));
    }

    REQUIRE (lines.size() == 6 + 7);
    for (std::size_t i = 1; i < lines.size(); ++i)
        CHECK (lines[i - 1]["seq"].get<std::uint64_t>() <= lines[i]["seq"].get<std::uint64_t>());
    CHECK (lines.back()["seq"] == 7);
}

TEST_CASE ("wait answers with an event after `since`, from the lookback or as it arrives", "[control][controller][wait]")
{
    auto tap = std::make_shared<EventTap>();
    Controller controller { tap };
    const auto client = controller.connect();
    tap->writeLine (logLine ("export.finished", Level::info, { { "path", "/a.wav" } }));
    tap->writeLine (logLine ("ui.other"));
    controller.pump (start);

    SECTION ("already in the lookback")
    {
        controller.receive (client, "{\"id\":1,\"op\":\"wait\",\"event\":\"export.finished\",\"since\":0}\n", start);
        controller.pump (start);

        const auto lines = takeLines (controller, client);
        REQUIRE (lines.size() == 1);
        CHECK (lines[0]["ok"] == true);
        CHECK (lines[0]["event_seq"] == 1);
        CHECK (lines[0]["event"]["event"] == "export.finished");
        CHECK (lines[0]["event"]["path"] == "/a.wav");
    }
    SECTION ("arriving later, without holding back later requests")
    {
        controller.receive (client, "{\"id\":1,\"op\":\"wait\",\"event\":\"export.finished\",\"timeout_ms\":1000}\n{\"id\":2,\"op\":\"hello\"}\n", start);
        controller.pump (start + std::chrono::milliseconds (10));
        auto lines = takeLines (controller, client);
        REQUIRE (lines.size() == 1);
        CHECK (lines[0]["id"] == 2);

        tap->writeLine (logLine ("export.finished", Level::info, { { "path", "/b.wav" } }));
        controller.pump (start + std::chrono::milliseconds (999));
        lines = takeLines (controller, client);
        REQUIRE (lines.size() == 1);
        CHECK (lines[0]["id"] == 1);
        CHECK (lines[0]["ok"] == true);
        CHECK (lines[0]["event_seq"] == 3);
        CHECK (lines[0]["seq"] == 3);
        CHECK (lines[0]["event"]["path"] == "/b.wav");
    }
}

TEST_CASE ("wait times out with control.wait_timed_out on the injected clock", "[control][controller][wait]")
{
    overdub::core::ScopedLogCapture capture;
    Controller controller { std::make_shared<EventTap>() };
    const auto client = controller.connect();

    controller.receive (client, "{\"id\":1,\"op\":\"wait\",\"event\":\"export.finished\",\"since\":4,\"timeout_ms\":250}\n", start);
    controller.pump (start + std::chrono::milliseconds (249));
    CHECK (controller.pendingOutput (client).empty());
    CHECK (controller.hasPendingWork (client));

    controller.pump (start + std::chrono::milliseconds (250));
    const auto lines = takeLines (controller, client);
    REQUIRE (lines.size() == 1);
    CHECK (lines[0]["ok"] == false);
    CHECK (lines[0]["error"]["code"] == "control.wait_timed_out");
    CHECK (lines[0]["error"]["fields"] == json { { "event", "export.finished" }, { "since", 4 }, { "timeout_ms", 250 } });
    CHECK (! controller.hasPendingWork (client));
}

TEST_CASE ("a wait's control.wait_timed_out line reaches a debug subscriber before the reply", "[control][controller][wait]")
{
    overdub::core::ScopedLogCapture capture;
    ProcessTap installed { std::make_shared<EventTap>() };
    Controller controller { installed.tap };
    const auto client = controller.connect();
    controller.receive (client, "{\"id\":1,\"op\":\"subscribe\",\"events\":[\"control.\"],\"level\":\"debug\"}\n", start);
    controller.pump (start);
    takeLines (controller, client);

    controller.receive (client, "{\"id\":2,\"op\":\"wait\",\"event\":\"export.finished\",\"timeout_ms\":250}\n", start);
    controller.pump (start + std::chrono::milliseconds (250));

    const auto lines = takeLines (controller, client);
    REQUIRE (lines.size() == 2);
    CHECK (lines[0]["event"]["event"] == "control.wait_timed_out");
    CHECK (lines[1]["id"] == 2);
    CHECK (lines[1]["error"]["code"] == "control.wait_timed_out");
    checkSeqNeverGoesBackwards (lines);
}

TEST_CASE ("a line over the limit is answered with control.line_too_long and closes the Connection", "[control][controller]")
{
    Controller controller { std::make_shared<EventTap>() };
    const auto client = controller.connect();

    controller.receive (client, std::string (Controller::maxLineBytes + 1, 'x'), start);
    controller.receive (client, "{\"id\":1,\"op\":\"hello\"}\n", start);
    controller.pump (start);

    const auto lines = takeLines (controller, client);
    REQUIRE (lines.size() == 1);
    CHECK (lines[0]["error"]["code"] == "control.line_too_long");
    CHECK (controller.closeReason (client) == CloseReason::lineTooLong);
}

TEST_CASE ("a subscriber that does not read loses events, is told how many, and still gets its replies", "[control][controller]")
{
    auto tap = std::make_shared<EventTap>();
    Controller controller { tap };
    const auto client = controller.connect();
    controller.receive (client, "{\"id\":1,\"op\":\"subscribe\"}\n", start);
    controller.pump (start);
    takeLines (controller, client);

    const auto padding = std::string (1000, 'p');
    const auto linesToOverflow = Controller::eventBudgetBytes / 1000 + 10;
    for (std::size_t i = 0; i < linesToOverflow; ++i)
        tap->writeLine (logLine ("ui.flood", Level::info, { { "padding", padding } }));
    controller.receive (client, "{\"id\":2,\"op\":\"hello\"}\n", start);
    controller.pump (start);

    auto lines = takeLines (controller, client);
    tap->writeLine (logLine ("ui.after"));
    controller.pump (start);
    for (auto& line : takeLines (controller, client))
        lines.push_back (std::move (line));

    std::size_t floodsDelivered = 0;
    std::uint64_t droppedReported = 0;
    std::size_t replies = 0;
    for (const auto& line : lines)
    {
        if (line.contains ("id"))
            ++replies;
        else if (line["event"]["event"] == "ui.flood")
            ++floodsDelivered;
        else if (line["event"]["event"] == "control.events_dropped")
            droppedReported += line["event"]["count"].get<std::uint64_t>();
    }
    checkSeqNeverGoesBackwards (lines);
    CHECK (replies == 1);
    CHECK (floodsDelivered < linesToOverflow);
    CHECK (droppedReported == linesToOverflow - floodsDelivered);
    CHECK (lines.back()["event"]["event"] == "ui.after");
}

TEST_CASE ("lines the event tap dropped are reported to subscribers as control.events_dropped", "[control][controller]")
{
    auto tap = std::make_shared<EventTap> (2);
    Controller controller { tap };
    const auto client = controller.connect();
    controller.receive (client, "{\"id\":1,\"op\":\"subscribe\"}\n", start);
    controller.pump (start);
    takeLines (controller, client);

    for (int i = 0; i < 5; ++i)
        tap->writeLine (logLine ("ui.burst"));
    controller.pump (start);

    const auto lines = takeLines (controller, client);
    REQUIRE (lines.size() == 3);
    CHECK (lines[0]["event"] == json { { "event", "control.events_dropped" }, { "count", 3 } });
    CHECK (lines[0]["seq"] == 3);
    CHECK (lines[1]["seq"] == 4);
    CHECK (lines[2]["seq"] == 5);
    checkSeqNeverGoesBackwards (lines);
}

TEST_CASE ("replies a client never reads past the outbox limit close it as too slow", "[control][controller]")
{
    const auto big = std::string (1u << 20, 'b');
    Controller controller { std::make_shared<EventTap>(), [&big] (const Request&) -> Reply { return json { { "blob", big } }; } };
    const auto client = controller.connect();

    for (int i = 0; i < 9; ++i)
        controller.receive (client, "{\"id\":" + std::to_string (i) + ",\"op\":\"big\"}\n", start);
    controller.pump (start);

    CHECK (controller.closeReason (client) == CloseReason::tooSlow);
    CHECK (controller.pendingOutput (client).empty());
}

namespace
{
StateReader activityReader (const std::string& kind)
{
    return [&kind] (const json& parts) -> Reply {
        if (! parts.is_null() && parts != json::array ({ "activity" }))
            return std::unexpected (overdub::core::Error { "control.bad_field", "no such part", { { "key", "parts" } } });
        return json { { "activity", { { "kind", kind } } } };
    };
}
} // namespace

TEST_CASE ("state answers what the reader reads, and unknown_op without a reader", "[control][controller][state]")
{
    overdub::core::ScopedLogCapture capture;
    std::string kind = "idle";
    Controller controller { std::make_shared<EventTap>(), {}, activityReader (kind) };
    Controller withoutState { std::make_shared<EventTap>() };
    const auto client = controller.connect();
    const auto other = withoutState.connect();

    controller.receive (client, "{\"id\":1,\"op\":\"state\",\"parts\":[\"activity\"]}\n{\"id\":2,\"op\":\"state\",\"parts\":[\"view\"]}\n", start);
    controller.pump (start);
    withoutState.receive (other, "{\"id\":3,\"op\":\"state\"}\n", start);
    withoutState.pump (start);

    const auto lines = takeLines (controller, client);
    REQUIRE (lines.size() == 2);
    CHECK (lines[0] == json { { "id", 1 }, { "ok", true }, { "seq", 0 }, { "activity", { { "kind", "idle" } } } });
    CHECK (lines[1]["error"]["code"] == "control.bad_field");
    CHECK (lines[1]["error"]["fields"]["key"] == "parts");
    const auto refused = takeLines (withoutState, other);
    REQUIRE (refused.size() == 1);
    CHECK (refused[0]["error"]["code"] == "control.unknown_op");
}

TEST_CASE ("a pointer wait answers on the first pump that reads the value, and at once when it already holds", "[control][controller][wait]")
{
    std::string kind = "export";
    Controller controller { std::make_shared<EventTap>(), {}, activityReader (kind) };
    const auto client = controller.connect();

    controller.receive (client, "{\"id\":1,\"op\":\"wait\",\"pointer\":\"/activity/kind\",\"equals\":\"idle\",\"timeout_ms\":1000}\n", start);
    controller.pump (start);
    CHECK (controller.pendingOutput (client).empty());
    CHECK (controller.hasPendingWork (client));

    kind = "idle";
    controller.pump (start + std::chrono::milliseconds (30));
    auto lines = takeLines (controller, client);
    REQUIRE (lines.size() == 1);
    CHECK (lines[0] == json { { "id", 1 }, { "ok", true }, { "seq", 0 }, { "pointer", "/activity/kind" }, { "value", "idle" } });
    CHECK (! controller.hasPendingWork (client));

    controller.receive (client, "{\"id\":2,\"op\":\"wait\",\"pointer\":\"/activity/kind\",\"equals\":\"idle\"}\n", start);
    controller.pump (start);
    lines = takeLines (controller, client);
    REQUIRE (lines.size() == 1);
    CHECK (lines[0] == json { { "id", 2 }, { "ok", true }, { "seq", 0 }, { "pointer", "/activity/kind" }, { "value", "idle" } });
}

TEST_CASE ("a pointer wait times out with the value it last read", "[control][controller][wait]")
{
    overdub::core::ScopedLogCapture capture;
    std::string kind = "export";
    Controller controller { std::make_shared<EventTap>(), {}, activityReader (kind) };
    const auto client = controller.connect();

    controller.receive (client, "{\"id\":1,\"op\":\"wait\",\"pointer\":\"/activity/kind\",\"equals\":\"idle\",\"timeout_ms\":250}\n", start);
    controller.pump (start + std::chrono::milliseconds (249));
    CHECK (controller.pendingOutput (client).empty());

    controller.pump (start + std::chrono::milliseconds (250));
    const auto lines = takeLines (controller, client);
    REQUIRE (lines.size() == 1);
    CHECK (lines[0]["error"]["code"] == "control.wait_timed_out");
    CHECK (lines[0]["error"]["fields"] == json { { "pointer", "/activity/kind" }, { "equals", "idle" }, { "value", "export" }, { "timeout_ms", 250 } });
    CHECK (! controller.hasPendingWork (client));
}

TEST_CASE ("a pointer wait with a malformed pointer, an unknown section or no equals is a bad field", "[control][controller][wait]")
{
    overdub::core::ScopedLogCapture capture;
    std::string kind = "idle";
    Controller controller { std::make_shared<EventTap>(), {}, activityReader (kind) };
    const auto client = controller.connect();

    controller.receive (client,
                        "{\"id\":1,\"op\":\"wait\",\"pointer\":\"activity\",\"equals\":\"idle\"}\n"
                        "{\"id\":2,\"op\":\"wait\",\"pointer\":\"/view/mode\",\"equals\":\"edit\"}\n"
                        "{\"id\":3,\"op\":\"wait\",\"pointer\":\"/activity/kind\"}\n"
                        "{\"id\":4,\"op\":\"wait\",\"pointer\":\"\",\"equals\":1}\n",
                        start);
    controller.pump (start);

    const auto lines = takeLines (controller, client);
    REQUIRE (lines.size() == 4);
    CHECK (lines[0]["error"]["fields"]["key"] == "pointer");
    CHECK (lines[1]["error"]["fields"]["key"] == "pointer");
    CHECK (lines[2]["error"]["fields"]["key"] == "equals");
    CHECK (lines[3]["error"]["fields"]["key"] == "pointer");
    for (const auto& line : lines)
        CHECK (line["error"]["code"] == "control.bad_field");
}
