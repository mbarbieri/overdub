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
    // Travel 200 px; positive pixels are towards louder. From -20 dB (position 0.5): 25 px louder
    // is position 0.625, -10 dB.
    CHECK_THAT (faderDragDb (-20.0, 25, 200), WithinAbs (-10.0, 1.0e-9));
    // 50 px quieter from -20 dB is position 0.25, -40 dB.
    CHECK_THAT (faderDragDb (-20.0, -50, 200), WithinAbs (-40.0, 1.0e-9));
    // From -20 dB, 7 px louder: position 0.535, -17.2 dB.
    CHECK_THAT (faderDragDb (-20.0, 7, 200), WithinAbs (-17.2, 1.0e-9));
    // From +6 dB (position 0.875), 1 px louder: position 0.88, +6.24 dB, stored as +6.2.
    CHECK_THAT (faderDragDb (6.0, 1, 200), WithinAbs (6.2, 1.0e-9));
    // Clamped at both ends.
    CHECK_THAT (faderDragDb (-20.0, 1000, 200), WithinAbs (12.0, 1.0e-9));
    CHECK_THAT (faderDragDb (-20.0, -1000, 200), WithinAbs (-60.0, 1.0e-9));
    // No travel: no change.
    CHECK_THAT (faderDragDb (-3.0, 40, 0), WithinAbs (-3.0, 1.0e-9));
}

TEST_CASE ("a fader drag that crosses 0 dB holds there for the detent's extra travel, then continues", "[ui][mixer]")
{
    REQUIRE (detentPixels == 8);
    // Travel 200 px. From -20 dB (position 0.5), 0 dB is 50 px louder.
    CHECK_THAT (faderDragDb (-20.0, 49, 200), WithinAbs (-0.4, 1.0e-9)); // short of it: unchanged
    CHECK_THAT (faderDragDb (-20.0, 50, 200), WithinAbs (0.0, 1.0e-9));
    CHECK_THAT (faderDragDb (-20.0, 54, 200), WithinAbs (0.0, 1.0e-9));
    CHECK_THAT (faderDragDb (-20.0, 58, 200), WithinAbs (0.0, 1.0e-9));
    // 59 px is 1 px past the detent: position 0.755, +0.24 dB.
    CHECK_THAT (faderDragDb (-20.0, 59, 200), WithinAbs (0.2, 1.0e-9));
    CHECK_THAT (faderDragDb (-20.0, 1000, 200), WithinAbs (12.0, 1.0e-9)); // every value still reachable

    // From +6 dB (position 0.875), 0 dB is 25 px quieter.
    CHECK_THAT (faderDragDb (6.0, -24, 200), WithinAbs (0.2, 1.0e-9)); // short of it: unchanged
    CHECK_THAT (faderDragDb (6.0, -25, 200), WithinAbs (0.0, 1.0e-9));
    CHECK_THAT (faderDragDb (6.0, -33, 200), WithinAbs (0.0, 1.0e-9));
    // 34 px is 1 px past the detent: position 0.745, -0.4 dB.
    CHECK_THAT (faderDragDb (6.0, -34, 200), WithinAbs (-0.4, 1.0e-9));
    CHECK_THAT (faderDragDb (6.0, -1000, 200), WithinAbs (-60.0, 1.0e-9));

    // A drag that starts at 0 dB holds there for the detent in either direction.
    CHECK_THAT (faderDragDb (0.0, 8, 200), WithinAbs (0.0, 1.0e-9));
    CHECK_THAT (faderDragDb (0.0, -8, 200), WithinAbs (0.0, 1.0e-9));
    CHECK_THAT (faderDragDb (0.0, 9, 200), WithinAbs (0.2, 1.0e-9));
    CHECK_THAT (faderDragDb (0.0, -9, 200), WithinAbs (-0.4, 1.0e-9));
}

TEST_CASE ("dragging the pan control moves pan across its width, from hard left to hard right", "[ui][mixer]")
{
    // Width 100 px spans -1 .. +1: 2 per 100 px.
    CHECK_THAT (panDrag (0.5, 25, 100), WithinAbs (1.0, 1.0e-9));
    CHECK_THAT (panDrag (-0.5, -25, 100), WithinAbs (-1.0, 1.0e-9));
    CHECK_THAT (panDrag (0.5, -10, 100), WithinAbs (0.3, 1.0e-9));
    CHECK_THAT (panDrag (0.9, 30, 100), WithinAbs (1.0, 1.0e-9));
    CHECK_THAT (panDrag (-0.9, -30, 100), WithinAbs (-1.0, 1.0e-9));
    // Rounded to hundredths.
    CHECK_THAT (panDrag (0.2, 1, 300), WithinAbs (0.21, 1.0e-9));
    CHECK_THAT (panDrag (0.2, 10, 0), WithinAbs (0.2, 1.0e-9));

    CHECK_THAT (panPosition (-1.0), WithinAbs (0.0, 1.0e-9));
    CHECK_THAT (panPosition (0.0), WithinAbs (0.5, 1.0e-9));
    CHECK_THAT (panPosition (0.5), WithinAbs (0.75, 1.0e-9));
    CHECK_THAT (panPosition (1.0), WithinAbs (1.0, 1.0e-9));
}

TEST_CASE ("a pan drag that crosses centre holds at C for the detent's extra travel, then continues", "[ui][mixer]")
{
    // Width 100 px (0.02 per px). From -0.5, centre is 25 px right.
    CHECK_THAT (panDrag (-0.5, 20, 100), WithinAbs (-0.1, 1.0e-9)); // short of it: unchanged
    CHECK_THAT (panDrag (-0.5, 25, 100), WithinAbs (0.0, 1.0e-9));
    CHECK_THAT (panDrag (-0.5, 33, 100), WithinAbs (0.0, 1.0e-9));
    CHECK_THAT (panDrag (-0.5, 34, 100), WithinAbs (0.02, 1.0e-9));
    CHECK_THAT (panDrag (-0.5, 100, 100), WithinAbs (1.0, 1.0e-9)); // hard right still reachable

    // From 0.5, centre is 25 px left.
    CHECK_THAT (panDrag (0.5, -25, 100), WithinAbs (0.0, 1.0e-9));
    CHECK_THAT (panDrag (0.5, -33, 100), WithinAbs (0.0, 1.0e-9));
    CHECK_THAT (panDrag (0.5, -40, 100), WithinAbs (-0.14, 1.0e-9));

    // From C, either direction.
    CHECK_THAT (panDrag (0.0, 8, 100), WithinAbs (0.0, 1.0e-9));
    CHECK_THAT (panDrag (0.0, -8, 100), WithinAbs (0.0, 1.0e-9));
    CHECK_THAT (panDrag (0.0, 10, 100), WithinAbs (0.04, 1.0e-9));
    CHECK_THAT (panDrag (0.0, -10, 100), WithinAbs (-0.04, 1.0e-9));
}

TEST_CASE ("an arrow key moves a fader by 1 dB (0.1 fine), stopping at 0 dB when it would cross it", "[ui][mixer]")
{
    CHECK_THAT (faderKeyStepDb (-6.0, true, false), WithinAbs (-5.0, 1.0e-9));
    CHECK_THAT (faderKeyStepDb (-6.0, false, false), WithinAbs (-7.0, 1.0e-9));
    CHECK_THAT (faderKeyStepDb (-6.0, true, true), WithinAbs (-5.9, 1.0e-9));
    CHECK_THAT (faderKeyStepDb (-0.3, true, false), WithinAbs (0.0, 1.0e-9));
    CHECK_THAT (faderKeyStepDb (0.5, false, false), WithinAbs (0.0, 1.0e-9));
    CHECK_THAT (faderKeyStepDb (0.05, false, true), WithinAbs (0.0, 1.0e-9));
    CHECK_THAT (faderKeyStepDb (0.0, true, false), WithinAbs (1.0, 1.0e-9));
    CHECK_THAT (faderKeyStepDb (0.0, false, true), WithinAbs (-0.1, 1.0e-9));
    CHECK_THAT (faderKeyStepDb (11.5, true, false), WithinAbs (12.0, 1.0e-9));
    CHECK_THAT (faderKeyStepDb (-59.5, false, false), WithinAbs (-60.0, 1.0e-9));
}

TEST_CASE ("an arrow key moves pan by 5% (1% fine), stopping at C when it would cross it", "[ui][mixer]")
{
    CHECK_THAT (panKeyStep (-0.5, true, false), WithinAbs (-0.45, 1.0e-9));
    CHECK_THAT (panKeyStep (-0.5, false, false), WithinAbs (-0.55, 1.0e-9));
    CHECK_THAT (panKeyStep (0.5, true, true), WithinAbs (0.51, 1.0e-9));
    CHECK_THAT (panKeyStep (0.03, false, false), WithinAbs (0.0, 1.0e-9));
    CHECK_THAT (panKeyStep (-0.02, true, false), WithinAbs (0.0, 1.0e-9));
    CHECK_THAT (panKeyStep (0.0, false, false), WithinAbs (-0.05, 1.0e-9));
    CHECK_THAT (panKeyStep (0.98, true, false), WithinAbs (1.0, 1.0e-9));
    CHECK_THAT (panKeyStep (-1.0, false, true), WithinAbs (-1.0, 1.0e-9));
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
