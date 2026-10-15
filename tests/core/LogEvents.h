#pragma once

#include "core/Log.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

// Looking up captured log events by name (tests assert `event` and fields, never `msg`).
namespace overdub::core::testing
{
// The first captured event named `name`; fails the test when there is none.
inline nlohmann::json firstEvent (const ScopedLogCapture& capture, const std::string& name)
{
    for (const auto& event : capture.events())
        if (event.at ("event") == name)
            return event;
    FAIL ("no event " << name);
    return {};
}
} // namespace overdub::core::testing
