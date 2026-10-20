#pragma once

#include "control/EventTap.h"
#include "core/Error.h"
#include "core/Log.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// The control protocol without file descriptors: bytes in, bytes out, per Connection. The Server
// (control/Server.h) moves bytes between sockets and this; tests drive it with strings. Message
// thread only.
//
// Protocol 1: newline-delimited JSON objects, UTF-8, one per line, at most maxLineBytes.
//   request  {"id": <number|string>, "op": "<op>", ...fields}
//   reply    {"id": .., "ok": true, "seq": n, ...result}
//            {"id": .., "ok": false, "seq": n, "error": {"code", "message", "fields"}}
//   event    {"seq": n, "event": <the log line object>}          only after `subscribe`
//            {"seq": n, "event": {"event": "control.events_dropped", "count": n}}
// Ops: hello, subscribe {events: [prefix...], level}, wait {event, since, timeout_ms}; any other op
// goes to the handler given at construction.
//
// Ordering: a reply is held until the next pump has drained the EventTap and delivered the lines
// to every Connection, and it carries the seq of the last line delivered. So on every Connection
// each event a request logged arrives before its reply, with a seq no greater than the reply's.
namespace overdub::control
{
using Clock = std::chrono::steady_clock;
using ConnectionId = std::uint64_t;

struct Request
{
    ConnectionId connection = 0;
    nlohmann::json id;     // number or string, echoed in the reply
    std::string op;
    nlohmann::json fields; // the request object without `id` and `op`
};

// The result object merged into an ok reply, or the Error an error reply carries.
using Reply = core::Expected<nlohmann::json>;
using OpHandler = std::function<Reply (const Request&)>;

enum class CloseReason
{
    lineTooLong,
    tooSlow
};
std::string_view closeReasonName (CloseReason reason);

// What a subscription lets through: lines at minLevel or above whose event starts with one of the
// prefixes (no prefixes: every event).
struct EventFilter
{
    std::vector<std::string> prefixes;
    core::Level minLevel = core::Level::info;
};

// `wait`'s conditions. #167 adds a JSON pointer into the app state equal to a value.
struct EventAfter
{
    std::string event; // exact event name
    std::uint64_t since = 0;
};
using WaitCondition = std::variant<EventAfter>;

class Controller
{
public:
    static constexpr int protocolVersion = 1;
    static constexpr std::size_t maxLineBytes = 1u << 20;
    // Unwritten replies past this close the Connection (control.client_too_slow).
    static constexpr std::size_t maxOutboxBytes = 8u << 20;
    // Events past this are dropped for that Connection and counted (control.events_dropped), so a
    // slow subscriber loses events before it loses replies.
    static constexpr std::size_t eventBudgetBytes = 4u << 20;
    static constexpr std::size_t lookbackLines = 1024;
    static constexpr std::int64_t defaultWaitMs = 30'000;
    static constexpr std::int64_t maxWaitMs = 10 * 60 * 1000;

    // `otherOps` answers every op that is not hello, subscribe or wait; the default answers
    // control.unknown_op.
    explicit Controller (std::shared_ptr<EventTap> tap, OpHandler otherOps = {});

    ConnectionId connect();
    void disconnect (ConnectionId connection);

    // Bytes read from the Connection, split anywhere. Complete lines run now, in order; their
    // replies are held until the next pump.
    void receive (ConnectionId connection, std::string_view bytes, Clock::time_point now);

    // Drains the EventTap to subscribers and waits, times out waits, then releases held replies.
    void pump (Clock::time_point now);

    // What the Connection should be sent next; the Server writes what it can and consumes that.
    std::string_view pendingOutput (ConnectionId connection) const;
    void consumeOutput (ConnectionId connection, std::size_t bytes);

    // Held replies, waits or unwritten output remain: a peer that finished sending still gets them.
    bool hasPendingWork (ConnectionId connection) const;

    // Set once the Connection must close after its pending output is written.
    std::optional<CloseReason> closeReason (ConnectionId connection) const;

private:
    struct DeliveredLine
    {
        std::uint64_t seq = 0;
        std::string event;
        core::Level level = core::Level::info;
        std::string line;
    };

    struct PendingWait
    {
        nlohmann::json requestId;
        WaitCondition until;
        Clock::time_point deadline;
        std::int64_t timeoutMs = 0;
    };

    struct HeldReply
    {
        nlohmann::json requestId;
        Reply reply;
    };

    struct Connection
    {
        std::string inbound;
        std::string outbox;
        std::vector<HeldReply> held;
        std::vector<PendingWait> waits;
        std::optional<EventFilter> subscription;
        std::uint64_t droppedEvents = 0;
        std::optional<CloseReason> closing;
    };

    void handleLine (ConnectionId connectionId, Connection& connection, std::string_view line, Clock::time_point now);
    std::optional<Reply> runOp (Connection& connection, const Request& request, Clock::time_point now);
    Reply subscribe (Connection& connection, const nlohmann::json& fields);
    std::optional<Reply> startWait (Connection& connection, const Request& request, Clock::time_point now);
    std::optional<Reply> matchWait (const PendingWait& wait, const DeliveredLine& line) const;

    void deliverEvent (Connection& connection, const DeliveredLine& line);
    void sendDroppedNotice (Connection& connection);
    void sendReply (Connection& connection, const HeldReply& held);

    std::shared_ptr<EventTap> tap;
    OpHandler otherOps;
    std::map<ConnectionId, Connection> connections;
    ConnectionId nextConnection = 1;
    std::deque<DeliveredLine> lookback; // the last lookbackLines delivered, oldest first
    std::uint64_t lastDeliveredSeq = 0;
};
} // namespace overdub::control
