#pragma once

#include <chrono>

namespace overdub::ui
{
// The macOS double-click speed (System Settings ▸ Mouse), which double-Space follows (issue #216).
std::chrono::milliseconds systemDoubleClickInterval();
} // namespace overdub::ui
