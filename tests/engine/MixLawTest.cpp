#include "engine/MixLaw.h"
#include "core/Decibels.h"
#include "support/RealtimeGuardCatch.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

// The Mixer's law as pure functions (issue #31). Expected values are worked by hand from the
// spec: centre = sqrt (1/2), -6 dB = 0.501187, +12 dB = 3.981072.
using namespace overdub::engine;
using Catch::Matchers::WithinAbs;
namespace song = overdub::song;

TEST_CASE ("the pan law gives exactly one side at the ends and -3 dB on each side at centre", "[engine][mixer]")
{
    CHECK (panGains (-1.0) == ChannelGains { 1.0f, 0.0f });
    CHECK (panGains (1.0) == ChannelGains { 0.0f, 1.0f });
    CHECK (panGains (-7.0) == ChannelGains { 1.0f, 0.0f });
    CHECK (panGains (7.0) == ChannelGains { 0.0f, 1.0f });

    const auto centre = panGains (0.0);
    CHECK_THAT (centre.left, WithinAbs (0.70710678, 1.0e-7));
    CHECK_THAT (centre.right, WithinAbs (0.70710678, 1.0e-7));
    CHECK_THAT (20.0 * std::log10 (centre.left), WithinAbs (-3.0103, 1.0e-4));

    // Constant power everywhere: left^2 + right^2 = 1.
    for (const auto pan : { -0.9, -0.5, -0.1, 0.25, 0.75 })
    {
        const auto gains = panGains (pan);
        CHECK_THAT (gains.left * gains.left + gains.right * gains.right, WithinAbs (1.0, 1.0e-6));
    }
    CHECK (panGains (-0.5).left > panGains (-0.5).right);
}

TEST_CASE ("volume in dB becomes its linear gain", "[engine][mixer]")
{
    song::Song song;
    song.master.volumeDb = 12.0;
    for (const auto volumeDb : { 0.0, -6.0, -60.0 })
        song.tracks.push_back (song::Track { .volumeDb = volumeDb, .pan = -1.0 });

    const auto gains = mixGains (song);
    CHECK_THAT (gains.tracks[0].left, WithinAbs (1.0, 0.0));
    CHECK_THAT (gains.tracks[1].left, WithinAbs (0.501187, 1.0e-6));
    CHECK_THAT (gains.tracks[2].left, WithinAbs (0.001, 1.0e-9));
    CHECK_THAT (gains.master, WithinAbs (3.981072, 1.0e-6));
}

TEST_CASE ("the volume law's dB conversion neither allocates nor locks", "[engine][mixer][rt]")
{
    auto volume = 0.0f;
    REQUIRE_REALTIME_SAFE (volume = (float) overdub::core::decibelsToGain (-6.0206));
    CHECK_THAT (volume, WithinAbs (0.5, 1.0e-6));
}

TEST_CASE ("mute silences a Track and any solo silences every Track that is not soloed", "[engine][mixer]")
{
    song::Track plain;
    song::Track muted;
    muted.mute = true;
    song::Track soloed;
    soloed.solo = true;
    song::Track soloedAndMuted;
    soloedAndMuted.solo = true;
    soloedAndMuted.mute = true;

    CHECK (isAudible (plain, false));
    CHECK (! isAudible (muted, false));
    CHECK (! isAudible (plain, true));
    CHECK (isAudible (soloed, true));
    CHECK (! isAudible (soloedAndMuted, true));
}

TEST_CASE ("a Song's mix gains combine volume, pan, mute, solo and the Master", "[engine][mixer]")
{
    auto mixed = song::makeEmptySong (48000);
    mixed.tracks.resize (3);
    mixed.tracks[0].volumeDb = -6.0;
    mixed.tracks[0].pan = -1.0;
    mixed.tracks[1].solo = true;
    mixed.tracks[1].pan = 1.0;
    mixed.tracks[2].volumeDb = 12.0;
    mixed.master.volumeDb = -6.0;

    SECTION ("with a solo, only the soloed Track sounds")
    {
        const auto gains = mixGains (mixed);
        REQUIRE (gains.tracks.size() == 3);
        CHECK (gains.tracks[0] == ChannelGains { 0.0f, 0.0f });
        CHECK (gains.tracks[1] == ChannelGains { 0.0f, 1.0f });
        CHECK (gains.tracks[2] == ChannelGains { 0.0f, 0.0f });
        CHECK_THAT (gains.master, WithinAbs (0.501187, 1.0e-6));
    }

    SECTION ("without it, each Track's volume times its pan")
    {
        mixed.tracks[1].solo = false;
        const auto gains = mixGains (mixed);
        CHECK_THAT (gains.tracks[0].left, WithinAbs (0.501187, 1.0e-6));
        CHECK_THAT (gains.tracks[0].right, WithinAbs (0.0, 0.0));
        CHECK_THAT (gains.tracks[2].left, WithinAbs (3.981072 * 0.70710678, 1.0e-5));
        CHECK_THAT (gains.tracks[2].right, WithinAbs (3.981072 * 0.70710678, 1.0e-5));
    }
}
