#include "ui/Follow.h"

#include <catch2/catch_test_macros.hpp>

using namespace overdub;
using namespace overdub::ui;

// Follow's geometry (issue #266): where Edit's lanes page to and where Transcribe's waveform
// scrolls to while the Song plays, from views, widths and playheads given in whole pixels.
namespace
{
// 100 samples per pixel across 1000 pixels: a view spans 100 000 samples.
constexpr SampleCount samplesPerPixel = 100;
constexpr int width = 1000;
constexpr SampleCount span = samplesPerPixel * width;

TimelineView viewFrom (SampleCount firstSample)
{
    return TimelineView { .firstSample = firstSample, .samplesPerPixel = samplesPerPixel, .laneX = 150 };
}
} // namespace

TEST_CASE ("Edit's lanes stay put while the playhead is inside them", "[ui][follow]")
{
    const auto view = viewFrom (200'000);
    CHECK (editFollowFirstSample (view, width, 200'000) == 200'000);
    CHECK (editFollowFirstSample (view, width, 250'000) == 200'000);
    CHECK (editFollowFirstSample (view, width, 200'000 + span - 1) == 200'000);
}

TEST_CASE ("Edit's lanes page forward to put the playhead a tenth of their width in", "[ui][follow]")
{
    const auto view = viewFrom (200'000);
    // A tenth of 1000 pixels is 100 pixels, 10 000 samples.
    CHECK (editFollowFirstSample (view, width, 200'000 + span) == 290'000);
    CHECK (editFollowFirstSample (view, width, 900'000) == 890'000);
}

TEST_CASE ("Edit's lanes page back to a playhead before them, never before the Song's start", "[ui][follow]")
{
    const auto view = viewFrom (200'000);
    CHECK (editFollowFirstSample (view, width, 199'999) == 189'999);
    CHECK (editFollowFirstSample (view, width, 50'000) == 40'000);
    CHECK (editFollowFirstSample (view, width, 5'000) == 0);
    CHECK (editFollowFirstSample (view, width, 0) == 0);
}

namespace
{
constexpr SampleCount songLength = 1'000'000;
// 20% of 1000 pixels is 200 pixels, 20 000 samples.
constexpr SampleCount spot = 20'000;

SampleCount transcribeFollow (SampleCount firstSample, SampleCount playhead, const song::Loop* loop = nullptr)
{
    return transcribeFollowFirstSample (viewFrom (firstSample), width, songLength, playhead, loop);
}

song::Loop loopOver (SampleCount start, SampleCount end)
{
    return song::Loop { .id = "loop", .name = "Loop 1", .start = start, .end = end };
}
} // namespace

TEST_CASE ("Transcribe's waveform scrolls to hold the playhead a fifth of its width in", "[ui][follow]")
{
    CHECK (transcribeFollow (0, 300'000) == 300'000 - spot);
    CHECK (transcribeFollow (250'000, 300'000) == 300'000 - spot);
    CHECK (transcribeFollow (250'000, 270'001) == 270'001 - spot);
}

TEST_CASE ("Transcribe's playhead walks from the Song's start, and from a view's left, up to its spot", "[ui][follow]")
{
    CHECK (transcribeFollow (0, 0) == 0);
    CHECK (transcribeFollow (0, spot - 1) == 0);
    CHECK (transcribeFollow (250'000, 260'000) == 250'000);
}

TEST_CASE ("Transcribe's waveform goes back to a playhead before it", "[ui][follow]")
{
    CHECK (transcribeFollow (500'000, 300'000) == 300'000 - spot);
    CHECK (transcribeFollow (500'000, 10'000) == 0);
}

TEST_CASE ("Transcribe's waveform stops at the Song's end, where the playhead walks to the right edge", "[ui][follow]")
{
    CHECK (transcribeFollow (850'000, 950'000) == songLength - span);
    CHECK (transcribeFollow (songLength - span, 999'999) == songLength - span);
}

TEST_CASE ("Transcribe's waveform never moves when the whole Song fits", "[ui][follow]")
{
    CHECK (transcribeFollowFirstSample (viewFrom (0), width, span - 1, 80'000, nullptr) == 0);
    CHECK (transcribeFollowFirstSample (viewFrom (0), width, span, 80'000, nullptr) == 0);
}

TEST_CASE ("A Loop that fits and shows whole keeps the waveform still", "[ui][follow]")
{
    const auto loop = loopOver (120'000, 160'000);
    CHECK (transcribeFollow (100'000, 150'000, &loop) == 100'000);
    CHECK (transcribeFollow (100'000, 120'000, &loop) == 100'000);
    // As wide as the view, showing whole.
    const auto exact = loopOver (100'000, 100'000 + span);
    CHECK (transcribeFollow (100'000, 190'000, &exact) == 100'000);
}

TEST_CASE ("A Loop that fits but shows in part moves the waveform the least that shows it whole", "[ui][follow]")
{
    const auto early = loopOver (80'000, 150'000);
    CHECK (transcribeFollow (100'000, 90'000, &early) == 80'000);
    const auto late = loopOver (150'000, 230'000);
    CHECK (transcribeFollow (100'000, 200'000, &late) == 230'000 - span);
    const auto offScreen = loopOver (600'000, 650'000);
    CHECK (transcribeFollow (100'000, 600'000, &offScreen) == 650'000 - span);
}

TEST_CASE ("A Loop wider than the waveform scrolls like the Song, its wrap going back to its start", "[ui][follow]")
{
    const auto wide = loopOver (100'000, 400'000);
    CHECK (transcribeFollow (250'000, 300'000, &wide) == 300'000 - spot);
    CHECK (transcribeFollow (300'000, 100'000, &wide) == 100'000 - spot);
}

TEST_CASE ("A playhead outside the Loop scrolls like the Song", "[ui][follow]")
{
    const auto loop = loopOver (120'000, 160'000);
    CHECK (transcribeFollow (100'000, 500'000, &loop) == 500'000 - spot);
    CHECK (transcribeFollow (100'000, 160'000, &loop) == 160'000 - spot);
}
