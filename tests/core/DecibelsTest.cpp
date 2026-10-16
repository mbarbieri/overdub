#include "core/Decibels.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

// Gain tolerance 1e-6 is the null-test residual of ADR 0005 (-120 dBFS).
using Catch::Matchers::WithinAbs;
using overdub::core::decibelsToGain;
using overdub::core::gainToDecibels;

TEST_CASE ("decibels become their linear gain", "[core][decibels]")
{
    CHECK_THAT (decibelsToGain (0.0), WithinAbs (1.0, 1.0e-6));
    CHECK_THAT (decibelsToGain (-6.0206), WithinAbs (0.5, 1.0e-6));
    CHECK_THAT (decibelsToGain (6.0206), WithinAbs (2.0, 1.0e-6));
    CHECK_THAT (decibelsToGain (-60.0), WithinAbs (0.001, 1.0e-9));
}

TEST_CASE ("a linear gain becomes its decibels", "[core][decibels]")
{
    CHECK_THAT (gainToDecibels (1.0), WithinAbs (0.0, 1.0e-9));
    CHECK_THAT (gainToDecibels (0.5), WithinAbs (-6.0206, 1.0e-6));
    CHECK_THAT (gainToDecibels (0.001), WithinAbs (-60.0, 1.0e-9));
    CHECK (std::isinf (gainToDecibels (0.0)));
    CHECK (gainToDecibels (0.0) < 0.0);
}

TEST_CASE ("decibels survive a round trip through linear gain", "[core][decibels]")
{
    for (const auto db : { -60.0, -24.0, -6.0206, -0.5, 0.0, 3.0, 12.0 })
    {
        INFO (db);
        CHECK_THAT (gainToDecibels (decibelsToGain (db)), WithinAbs (db, 1.0e-9));
    }
    for (const auto gain : { 0.001, 0.25, 0.5, 1.0, 3.981072 })
    {
        INFO (gain);
        CHECK_THAT (decibelsToGain (gainToDecibels (gain)), WithinAbs (gain, 1.0e-6));
    }
}
