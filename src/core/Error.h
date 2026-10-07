#pragma once

#include "core/Log.h"

#include <expected>
#include <string>

// Explicit errors (ADR 0006). Failures travel as Expected<T>; an Error is logged once, where it is
// handled, via logError. Never use Expected or OVERDUB_CHECK on the audio thread: push an RtEvent.
namespace overdub::core
{
struct Error
{
    std::string code;    // "area.snake_case", the same namespace as event names
    std::string message; // for humans; tests never assert on it
    Fields fields;       // flat JSON object (or null)
};

template <typename T>
using Expected = std::expected<T, Error>;

// Logs the Error as one event: event = code, msg = message, plus its fields.
void logError (const Error& error, Level level = Level::error);

namespace detail
{
// Logs the failed invariant and returns false. Used only by OVERDUB_CHECK.
bool reportFailedCheck (const char* condition, const char* event, const char* file, int line, const Fields& fields);
} // namespace detail
} // namespace overdub::core

// OVERDUB_CHECK (cond, "area.event", {"key", value}, ...) evaluates to cond as a bool. When cond is
// false it logs an `error` event with the fields plus `condition` and `where` (file:line) in every
// build (a caller field named `condition` or `where` is written as field_<key>), asserts in debug
// builds, and lets the caller recover in release:
//     if (! OVERDUB_CHECK (count > 0, "song.empty_lane", {"lane", laneId})) return;
#define OVERDUB_CHECK(cond, event, ...) \
    (static_cast<bool> (cond)           \
         ? true                         \
         : ::overdub::core::detail::reportFailedCheck (#cond, event, __FILE__, __LINE__, ::nlohmann::json::object ({ __VA_ARGS__ })))
