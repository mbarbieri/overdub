#pragma once

#include <string>

namespace overdub::core
{
// Facts about this binary, fixed at compile time.
struct BuildInfo
{
    std::string version;      // CMake PROJECT_VERSION, e.g. "0.1.0"
    std::string architecture; // "x86_64" or "arm64"
};

BuildInfo currentBuildInfo();
} // namespace overdub::core
