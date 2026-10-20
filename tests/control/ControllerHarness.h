#pragma once

#include "control/Controller.h"
#include "core/Log.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace overdub::control::testing
{
inline std::vector<nlohmann::json> takeLines (Controller& controller, ConnectionId connection)
{
    const auto output = std::string (controller.pendingOutput (connection));
    controller.consumeOutput (connection, output.size());
    std::vector<nlohmann::json> lines;
    std::size_t start = 0;
    for (auto end = output.find ('\n'); end != std::string::npos; end = output.find ('\n', start))
    {
        lines.push_back (nlohmann::json::parse (output.substr (start, end - start)));
        start = end + 1;
    }
    CHECK (start == output.size());
    return lines;
}

struct ProcessTap
{
    explicit ProcessTap (std::shared_ptr<EventTap> installed)
        : tap (std::move (installed))
    {
        core::processLogger().addTap (tap);
    }
    ~ProcessTap() { core::processLogger().removeTap (tap); }

    std::shared_ptr<EventTap> tap;
};
} // namespace overdub::control::testing
