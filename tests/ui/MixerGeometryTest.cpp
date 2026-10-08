#include "ui/MixerGeometry.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

// The Mixer's controls as pure mappings (issue #31, ADR 0005 UI amendment). Worked by hand from the
// fader law: the lower three quarters of the travel are -60 .. 0 dB, linear in dB (80 dB per
// travel), the top quarter is 0 .. +12 dB (48 dB per travel).
using namespace overdub::ui;
using Catch::Matchers::WithinAbs;

TEST_CASE ("fader position maps to dB and back", "[ui][mixer]")
{
    CHECK_THAT (faderPositionToDb (0.0), WithinAbs (-60.0, 1.0e-9));
    CHECK_THAT (faderPositionToDb (0.375), WithinAbs (-30.0, 1.0e-9));
    CHECK_THAT (faderPositionToDb (0.75), WithinAbs (0.0, 1.0e-9));
    CHECK_THAT (faderPositionToDb (0.875), WithinAbs (6.0, 1.0e-9));
    CHECK_THAT (faderPositionToDb (1.0), WithinAbs (12.0, 1.0e-9));
    CHECK_THAT (faderPositionToDb (-1.0), WithinAbs (-60.0, 1.0e-9));
    CHECK_THAT (faderPositionToDb (2.0), WithinAbs (12.0, 1.0e-9));

    CHECK_THAT (dbToFaderPosition (-60.0), WithinAbs (0.0, 1.0e-9));
    CHECK_THAT (dbToFaderPosition (-20.0), WithinAbs (0.5, 1.0e-9));
    CHECK_THAT (dbToFaderPosition (0.0), WithinAbs (0.75, 1.0e-9));
    CHECK_THAT (dbToFaderPosition (3.0), WithinAbs (0.8125, 1.0e-9));
    CHECK_THAT (dbToFaderPosition (-200.0), WithinAbs (0.0, 1.0e-9));
    CHECK_THAT (dbToFaderPosition (50.0), WithinAbs (1.0, 1.0e-9));

    for (const auto db : { -59.5, -42.0, -6.0, -0.1, 0.4, 7.5, 11.9 })
        CHECK_THAT (faderPositionToDb (dbToFaderPosition (db)), WithinAbs (db, 1.0e-9));
}

TEST_CASE ("dragging the fader moves its position by the dragged share of its travel, in tenths of a dB", "[ui][mixer]")
{
    // Travel 200 px. From 0 dB (position 0.75): 25 px up is position 0.875, +6 dB.
    CHECK_THAT (faderDragDb (0.0, -25, 200), WithinAbs (6.0, 1.0e-9));
    // 50 px down is position 0.5, -20 dB.
    CHECK_THAT (faderDragDb (0.0, 50, 200), WithinAbs (-20.0, 1.0e-9));
    // From -20 dB, 7 px up: position 0.535, -17.2 dB.
    CHECK_THAT (faderDragDb (-20.0, -7, 200), WithinAbs (-17.2, 1.0e-9));
    // Rounded to a tenth: 1 px up from 0 dB is +0.24 dB, shown and stored as +0.2.
    CHECK_THAT (faderDragDb (0.0, -1, 200), WithinAbs (0.2, 1.0e-9));
    // Clamped at both ends.
    CHECK_THAT (faderDragDb (0.0, -1000, 200), WithinAbs (12.0, 1.0e-9));
    CHECK_THAT (faderDragDb (0.0, 1000, 200), WithinAbs (-60.0, 1.0e-9));
    // No travel: no change.
    CHECK_THAT (faderDragDb (-3.0, 40, 0), WithinAbs (-3.0, 1.0e-9));
}

TEST_CASE ("dragging the pan control moves pan across its width, from hard left to hard right", "[ui][mixer]")
{
    // Width 100 px spans -1 .. +1: 2 per 100 px.
    CHECK_THAT (panDrag (0.0, 25, 100), WithinAbs (0.5, 1.0e-9));
    CHECK_THAT (panDrag (0.0, -50, 100), WithinAbs (-1.0, 1.0e-9));
    CHECK_THAT (panDrag (0.5, -10, 100), WithinAbs (0.3, 1.0e-9));
    CHECK_THAT (panDrag (0.9, 30, 100), WithinAbs (1.0, 1.0e-9));
    CHECK_THAT (panDrag (-0.9, -30, 100), WithinAbs (-1.0, 1.0e-9));
    // Rounded to hundredths.
    CHECK_THAT (panDrag (0.0, 1, 300), WithinAbs (0.01, 1.0e-9));
    CHECK_THAT (panDrag (0.2, 10, 0), WithinAbs (0.2, 1.0e-9));

    CHECK_THAT (panPosition (-1.0), WithinAbs (0.0, 1.0e-9));
    CHECK_THAT (panPosition (0.0), WithinAbs (0.5, 1.0e-9));
    CHECK_THAT (panPosition (0.5), WithinAbs (0.75, 1.0e-9));
    CHECK_THAT (panPosition (1.0), WithinAbs (1.0, 1.0e-9));
}

TEST_CASE ("pan and volume read as short texts", "[ui][mixer]")
{
    CHECK (panText (0.0) == "C");
    CHECK (panText (0.004) == "C");
    CHECK (panText (-1.0) == "L100");
    CHECK (panText (-0.25) == "L25");
    CHECK (panText (0.5) == "R50");
    CHECK (volumeText (0.0) == "0.0 dB");
    CHECK (volumeText (-6.26) == "-6.3 dB");
    CHECK (volumeText (4.5) == "+4.5 dB");
}

TEST_CASE ("a meter shows a peak on the fader's scale and falls back slowly", "[ui][mixer][meters]")
{
    CHECK_THAT (meterPosition (1.0f), WithinAbs (0.75, 1.0e-6));   // 0 dBFS lines up with 0 dB on the fader
    CHECK_THAT (meterPosition (0.1f), WithinAbs (0.5, 1.0e-6));    // -20 dBFS
    CHECK_THAT (meterPosition (0.0f), WithinAbs (0.0, 1.0e-9));    // silence
    CHECK_THAT (meterPosition (1.0e-5f), WithinAbs (0.0, 1.0e-9)); // below -60 dBFS
    CHECK_THAT (meterPosition (10.0f), WithinAbs (1.0, 1.0e-9));   // +20 dBFS: over the top

    // A new peak shows at once; with none, the shown level falls by meterFallPerPoll each poll.
    CHECK_THAT (nextMeterLevel (0.2f, 0.5f), WithinAbs (0.5, 1.0e-9));
    CHECK_THAT (nextMeterLevel (0.5f, 0.0f), WithinAbs (0.5 * meterFallPerPoll, 1.0e-7));
    CHECK_THAT (nextMeterLevel (0.5f, 0.3f), WithinAbs (std::max (0.3, 0.5 * meterFallPerPoll), 1.0e-7));
}
