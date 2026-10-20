#include "control/Controller.h"

#include "core/BuildInfo.h"

#include <unistd.h>

#include <algorithm>
#include <utility>

namespace overdub::control
{
namespace
{
// Errors the protocol itself raises are answered to the client and logged at debug, once.
core::Error protocolError (std::string code, std::string message, nlohmann::json fields)
{
    core::Error error { std::move (code), std::move (message), std::move (fields) };
    core::logError (error, core::Level::debug);
    return error;
}

Reply unknownOp (const Request& request)
{
    return std::unexpected (protocolError ("control.unknown_op", "No such op", { { "op", request.op } }));
}

Reply badField (std::string_view key, std::string message)
{
    return std::unexpected (protocolError ("control.bad_field", std::move (message), { { "key", key } }));
}

std::string dumpLine (const nlohmann::ordered_json& object)
{
    return object.dump (-1, ' ', false, nlohmann::json::error_handler_t::replace) + "\n";
}

bool passesFilter (const EventFilter& filter, std::string_view event, core::Level level)
{
    if (static_cast<int> (level) < static_cast<int> (filter.minLevel))
        return false;
    if (filter.prefixes.empty())
        return true;
    return std::ranges::any_of (filter.prefixes, [event] (const std::string& prefix) { return event.starts_with (prefix); });
}

nlohmann::json parsedLine (const std::string& line)
{
    auto parsed = nlohmann::json::parse (line, nullptr, false);
    return parsed.is_discarded() ? nlohmann::json (line) : parsed;
}
} // namespace

std::string_view closeReasonName (CloseReason reason)
{
    switch (reason)
    {
        case CloseReason::lineTooLong: return "line_too_long";
        case CloseReason::tooSlow: return "client_too_slow";
    }
    return "client_too_slow";
}

Controller::Controller (std::shared_ptr<EventTap> eventTap, OpHandler handler)
    : tap (std::move (eventTap)),
      otherOps (handler ? std::move (handler) : OpHandler (unknownOp))
{
}

ConnectionId Controller::connect()
{
    const auto id = nextConnection++;
    connections.emplace (id, Connection {});
    return id;
}

void Controller::disconnect (ConnectionId connection)
{
    connections.erase (connection);
}

void Controller::receive (ConnectionId connectionId, std::string_view bytes, Clock::time_point now)
{
    const auto found = connections.find (connectionId);
    if (found == connections.end())
        return;
    auto& connection = found->second;
    if (connection.closing.has_value())
        return;

    connection.inbound.append (bytes);
    std::size_t start = 0;
    auto tooLong = false;
    for (auto end = connection.inbound.find ('\n'); end != std::string::npos; end = connection.inbound.find ('\n', start))
    {
        if (end - start > maxLineBytes)
        {
            tooLong = true;
            break;
        }
        const auto line = std::string (connection.inbound, start, end - start);
        start = end + 1;
        handleLine (connectionId, connection, line, now);
        if (connection.closing.has_value())
            return;
    }
    connection.inbound.erase (0, start);

    if (tooLong || connection.inbound.size() > maxLineBytes)
    {
        connection.inbound.clear();
        connection.held.push_back (HeldReply {
            nullptr,
            std::unexpected (protocolError ("control.line_too_long", "A request line is longer than the limit", { { "max_bytes", maxLineBytes } })) });
        connection.closing = CloseReason::lineTooLong;
    }
}

void Controller::handleLine (ConnectionId connectionId, Connection& connection, std::string_view line, Clock::time_point now)
{
    if (line.ends_with ('\r'))
        line.remove_suffix (1);
    if (line.empty())
        return;

    auto json = nlohmann::json::parse (line, nullptr, false);
    if (json.is_discarded())
        return connection.held.push_back ({ nullptr, std::unexpected (protocolError ("control.bad_json", "The line is not JSON", {})) });
    if (! json.is_object())
        return connection.held.push_back (
            { nullptr, std::unexpected (protocolError ("control.bad_request", "A request is a JSON object", { { "key", "" } })) });

    auto id = json.contains ("id") ? json["id"] : nlohmann::json();
    if (! id.is_number() && ! id.is_string())
        return connection.held.push_back (
            { nullptr, std::unexpected (protocolError ("control.bad_request", "A request needs a number or string id", { { "key", "id" } })) });
    if (! json.contains ("op") || ! json["op"].is_string())
        return connection.held.push_back (
            { id, std::unexpected (protocolError ("control.bad_request", "A request needs a string op", { { "key", "op" } })) });

    Request request { .connection = connectionId, .id = id, .op = json["op"].get<std::string>(), .fields = json };
    request.fields.erase ("id");
    request.fields.erase ("op");

    if (auto reply = runOp (connection, request, now))
        connection.held.push_back ({ std::move (id), std::move (*reply) });
}

std::optional<Reply> Controller::runOp (Connection& connection, const Request& request, Clock::time_point now)
{
    if (request.op == "hello")
        return nlohmann::json {
            { "protocol", protocolVersion },
            { "app", "Overdub" },
            { "version", core::currentBuildInfo().version },
            { "pid", static_cast<int> (::getpid()) },
        };
    if (request.op == "subscribe")
        return subscribe (connection, request.fields);
    if (request.op == "wait")
        return startWait (connection, request, now);
    return otherOps (request);
}

Reply Controller::subscribe (Connection& connection, const nlohmann::json& fields)
{
    EventFilter filter;
    if (fields.contains ("events"))
    {
        const auto& events = fields["events"];
        if (! events.is_array() || ! std::ranges::all_of (events, [] (const nlohmann::json& prefix) { return prefix.is_string(); }))
            return badField ("events", "events is a list of event name prefixes");
        for (const auto& prefix : events)
            filter.prefixes.push_back (prefix.get<std::string>());
    }
    if (fields.contains ("level"))
    {
        const auto level = fields["level"].is_string() ? core::parseLevel (fields["level"].get<std::string>()) : std::nullopt;
        if (! level.has_value())
            return badField ("level", "level is debug, info, warn or error");
        filter.minLevel = *level;
    }
    connection.subscription = std::move (filter);
    connection.droppedEvents = 0;
    return nlohmann::json::object();
}

std::optional<Reply> Controller::startWait (Connection& connection, const Request& request, Clock::time_point now)
{
    const auto& fields = request.fields;
    if (! fields.contains ("event") || ! fields["event"].is_string() || fields["event"].get<std::string>().empty())
        return badField ("event", "event is the event name to wait for");

    auto since = lastDeliveredSeq;
    if (fields.contains ("since"))
    {
        if (! fields["since"].is_number_unsigned())
            return badField ("since", "since is a seq number");
        since = fields["since"].get<std::uint64_t>();
    }

    auto timeoutMs = defaultWaitMs;
    if (fields.contains ("timeout_ms"))
    {
        const auto& timeout = fields["timeout_ms"];
        if (! timeout.is_number_integer() || timeout.get<std::int64_t>() < 0 || timeout.get<std::int64_t>() > maxWaitMs)
            return badField ("timeout_ms", "timeout_ms is a whole number of milliseconds up to ten minutes");
        timeoutMs = timeout.get<std::int64_t>();
    }

    PendingWait wait {
        .requestId = request.id,
        .until = EventAfter { .event = fields["event"].get<std::string>(), .since = since },
        .deadline = now + std::chrono::milliseconds (timeoutMs),
        .timeoutMs = timeoutMs,
    };
    for (const auto& line : lookback)
        if (auto reply = matchWait (wait, line))
            return reply;

    connection.waits.push_back (std::move (wait));
    return std::nullopt;
}

std::optional<Reply> Controller::matchWait (const PendingWait& wait, const DeliveredLine& line) const
{
    return std::visit (
        [&line] (const EventAfter& condition) -> std::optional<Reply> {
        if (line.seq <= condition.since || line.event != condition.event)
            return std::nullopt;
        return nlohmann::json { { "event", parsedLine (line.line) }, { "event_seq", line.seq } };
    },
        wait.until);
}

void Controller::pump (Clock::time_point now)
{
    deliver (tap->drain());
    expireWaits (now);
    // A second drain delivers the control.wait_timed_out lines just logged, so they too precede
    // their replies.
    deliver (tap->drain());

    for (auto& [id, connection] : connections)
        for (const auto& held : std::exchange (connection.held, {}))
            sendReply (connection, held);
}

void Controller::deliver (TappedBatch batch)
{
    std::vector<DeliveredLine> delivered;
    delivered.reserve (batch.lines.size());
    for (auto& tapped : batch.lines)
    {
        const auto parsed = nlohmann::json::parse (tapped.line, nullptr, false);
        const auto event = parsed.is_object() ? parsed.value ("event", "") : std::string();
        const auto level = parsed.is_object() ? core::parseLevel (parsed.value ("lvl", "")) : std::nullopt;
        delivered.push_back ({ .seq = tapped.seq, .event = event, .level = level.value_or (core::Level::info), .line = std::move (tapped.line) });
        lastDeliveredSeq = tapped.seq;
    }

    for (auto& [id, connection] : connections)
    {
        if (connection.closing == CloseReason::tooSlow)
            continue;

        if (connection.subscription.has_value())
        {
            connection.droppedEvents += batch.dropped;
            for (const auto& line : delivered)
                if (passesFilter (*connection.subscription, line.event, line.level))
                    deliverEvent (connection, line);
            sendDroppedNotice (connection, lastDeliveredSeq);
        }

        std::erase_if (connection.waits, [&] (const PendingWait& wait) {
            for (const auto& line : delivered)
                if (auto reply = matchWait (wait, line))
                {
                    connection.held.push_back ({ wait.requestId, std::move (*reply) });
                    return true;
                }
            return false;
        });
    }

    for (auto& line : delivered)
        lookback.push_back (std::move (line));
    while (lookback.size() > lookbackLines)
        lookback.pop_front();
}

void Controller::expireWaits (Clock::time_point now)
{
    for (auto& [id, connection] : connections)
        std::erase_if (connection.waits, [&] (const PendingWait& wait) {
            if (now < wait.deadline)
                return false;
            const auto& condition = std::get<EventAfter> (wait.until);
            connection.held.push_back ({ wait.requestId,
                                         std::unexpected (protocolError ("control.wait_timed_out",
                                                                         "The event did not arrive in time",
                                                                         { { "event", condition.event },
                                                                           { "since", condition.since },
                                                                           { "timeout_ms", wait.timeoutMs } })) });
            return true;
        });
}

void Controller::deliverEvent (Connection& connection, const DeliveredLine& line)
{
    auto text = "{\"seq\":" + std::to_string (line.seq) + ",\"event\":" + line.line + "}\n";
    if (connection.outbox.size() + text.size() > eventBudgetBytes)
    {
        ++connection.droppedEvents;
        return;
    }
    // Every line dropped so far came before this one.
    sendDroppedNotice (connection, line.seq - 1);
    connection.outbox += text;
}

void Controller::sendDroppedNotice (Connection& connection, std::uint64_t seq)
{
    if (connection.droppedEvents == 0 || connection.outbox.size() >= eventBudgetBytes)
        return;
    nlohmann::ordered_json notice;
    notice["seq"] = seq;
    notice["event"] = { { "event", "control.events_dropped" }, { "count", connection.droppedEvents } };
    connection.outbox += dumpLine (notice);
    connection.droppedEvents = 0;
}

void Controller::sendReply (Connection& connection, const HeldReply& held)
{
    if (connection.closing == CloseReason::tooSlow)
        return;

    nlohmann::ordered_json reply;
    reply["id"] = held.requestId;
    reply["ok"] = held.reply.has_value();
    reply["seq"] = lastDeliveredSeq;
    if (held.reply.has_value())
    {
        if (held.reply->is_object())
            for (const auto& [key, value] : held.reply->items())
                if (! reply.contains (key))
                    reply[key] = value;
    }
    else
    {
        const auto& error = held.reply.error();
        reply["error"] = { { "code", error.code }, { "message", error.message }, { "fields", error.fields.is_null() ? nlohmann::json::object() : error.fields } };
    }

    connection.outbox += dumpLine (reply);
    if (connection.outbox.size() > maxOutboxBytes)
    {
        connection.outbox.clear();
        connection.closing = CloseReason::tooSlow;
    }
}

std::string_view Controller::pendingOutput (ConnectionId connection) const
{
    const auto found = connections.find (connection);
    return found == connections.end() ? std::string_view() : std::string_view (found->second.outbox);
}

void Controller::consumeOutput (ConnectionId connection, std::size_t bytes)
{
    if (const auto found = connections.find (connection); found != connections.end())
        found->second.outbox.erase (0, bytes);
}

bool Controller::hasPendingWork (ConnectionId connection) const
{
    const auto found = connections.find (connection);
    if (found == connections.end())
        return false;
    const auto& state = found->second;
    return ! state.held.empty() || ! state.waits.empty() || ! state.outbox.empty();
}

std::optional<CloseReason> Controller::closeReason (ConnectionId connection) const
{
    const auto found = connections.find (connection);
    return found == connections.end() ? std::nullopt : found->second.closing;
}
} // namespace overdub::control
