#include "core/BuildInfo.h"

#include <catch2/catch_test_macros.hpp>

#include <regex>

TEST_CASE ("build info reports the project version and an x86_64 architecture", "[core]")
{
    const auto info = overdub::core::currentBuildInfo();

    CHECK (info.version == "0.1.0");
    CHECK (info.architecture == "x86_64");
}

TEST_CASE ("build info reports the git commit the binary was built from", "[core]")
{
    const auto info = overdub::core::currentBuildInfo();

    // A short SHA, with "-dirty" when tracked files had uncommitted changes.
    CHECK (std::regex_match (info.gitSha, std::regex (R"([0-9a-f]{7,40}(-dirty)?)")));
}
