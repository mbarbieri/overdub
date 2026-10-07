#include "core/BuildInfo.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE ("build info reports the project version and an x86_64 architecture", "[core]")
{
    const auto info = overdub::core::currentBuildInfo();

    CHECK (info.version == "0.1.0");
    CHECK (info.architecture == "x86_64");
}
