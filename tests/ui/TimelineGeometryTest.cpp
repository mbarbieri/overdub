#include "ui/TimelineGeometry.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

using namespace overdub::ui;
using overdub::song::Clip;
using overdub::song::ClipSourceKind;
using overdub::song::PeakRange;
using overdub::song::Peaks;
using overdub::song::SampleCount;
using overdub::song::Song;

namespace
{
Song songWithClips (std::vector<Clip> clips, SampleCount takeLength = 100000)
{
    Song song = overdub::song::makeEmptySong (48000);
    overdub::song::Take take;
    take.id = "take";
    take.lengthSamples = takeLength;
    song.takes.push_back (take);
    overdub::song::Track track;
    track.id = "t";
    track.clips = std::move (clips);
    song.tracks.push_back (track);
    return song;
}

Clip clip (const std::string& id, SampleCount start, SampleCount length, SampleCount offset = 0)
{
    return Clip { .id = id, .source = { ClipSourceKind::take, "take" }, .timelineStart = start, .sourceOffset = offset, .length = length };
}
} // namespace

TEST_CASE ("sample and x convert both ways at every zoom", "[ui][timeline]")
{
    const auto samplesPerPixel = GENERATE (minSamplesPerPixel, SampleCount { 256 }, SampleCount { 1024 }, maxSamplesPerPixel);
    const TimelineView view { .firstSample = 10 * samplesPerPixel, .samplesPerPixel = samplesPerPixel, .laneX = 150 };
    INFO ("samples per pixel " << samplesPerPixel);

    CHECK (xForSample (view, view.firstSample) == 150);
    CHECK (xForSample (view, view.firstSample + 37 * samplesPerPixel) == 187);
    CHECK (xForSample (view, 0) == 140);
    CHECK (sampleForX (view, 150) == view.firstSample);
    CHECK (sampleForX (view, 187) == view.firstSample + 37 * samplesPerPixel);
    CHECK (sampleForX (view, 140) == 0);
    // A sample inside a pixel belongs to that pixel.
    CHECK (xForSample (view, view.firstSample + 37 * samplesPerPixel + samplesPerPixel - 1) == 187);
    CHECK (xForSample (view, view.firstSample - 1) == 149);
}

TEST_CASE ("zooming halves or doubles the samples per pixel, within the limits", "[ui][timeline]")
{
    const TimelineView view { .firstSample = 4800, .samplesPerPixel = 512, .laneX = 0 };
    CHECK (zoomedIn (view).samplesPerPixel == 256);
    CHECK (zoomedOut (view).samplesPerPixel == 1024);
    CHECK (zoomedIn (view).firstSample == 4800);

    CHECK (zoomedIn (TimelineView { .samplesPerPixel = minSamplesPerPixel }).samplesPerPixel == minSamplesPerPixel);
    CHECK (zoomedOut (TimelineView { .samplesPerPixel = maxSamplesPerPixel }).samplesPerPixel == maxSamplesPerPixel);
}

TEST_CASE ("hit-testing finds a Clip's body and its edges, top layer first", "[ui][timeline]")
{
    // 100 samples per pixel from x = 0: "low" spans x [0, 60), "high" spans x [20, 40).
    const TimelineView view { .firstSample = 0, .samplesPerPixel = 100, .laneX = 0 };
    const std::vector<Clip> clips { clip ("low", 0, 6000), clip ("high", 2000, 2000) };

    CHECK (hitTestClips (clips, view, 30) == ClipHit { "high", ClipPart::body });
    CHECK (hitTestClips (clips, view, 20) == ClipHit { "high", ClipPart::startEdge });
    CHECK (hitTestClips (clips, view, 20 + clipEdgeGrabWidth - 1) == ClipHit { "high", ClipPart::startEdge });
    CHECK (hitTestClips (clips, view, 20 + clipEdgeGrabWidth) == ClipHit { "high", ClipPart::body });
    CHECK (hitTestClips (clips, view, 39) == ClipHit { "high", ClipPart::endEdge });
    CHECK (hitTestClips (clips, view, 40 - clipEdgeGrabWidth) == ClipHit { "high", ClipPart::endEdge });
    CHECK (hitTestClips (clips, view, 45) == ClipHit { "low", ClipPart::body });
    CHECK (hitTestClips (clips, view, 59) == ClipHit { "low", ClipPart::endEdge });
    CHECK (! hitTestClips (clips, view, 60).has_value());
    CHECK (! hitTestClips (clips, view, -1).has_value());
}

TEST_CASE ("a narrow Clip's edge zones shrink so its body can still be grabbed", "[ui][timeline]")
{
    const TimelineView view { .firstSample = 0, .samplesPerPixel = 100, .laneX = 0 };
    const std::vector<Clip> clips { clip ("narrow", 1000, 900) }; // x [10, 19)
    CHECK (hitTestClips (clips, view, 10) == ClipHit { "narrow", ClipPart::startEdge });
    CHECK (hitTestClips (clips, view, 14) == ClipHit { "narrow", ClipPart::body });
    CHECK (hitTestClips (clips, view, 18) == ClipHit { "narrow", ClipPart::endEdge });
}

TEST_CASE ("dragging a Clip's body moves it by the dragged pixels in samples, not before 0", "[ui][timeline]")
{
    const auto song = songWithClips ({ clip ("c", 10000, 5000, 2000) });
    const TimelineView view { .firstSample = 0, .samplesPerPixel = 256, .laneX = 150 };
    const ClipDrag drag { .clipId = "c", .part = ClipPart::body, .downX = 300 };

    const auto right = songForDrag (song, drag, view, 340);
    REQUIRE (right.has_value());
    CHECK (right->tracks[0].clips[0].timelineStart == 10000 + 40 * 256);
    CHECK (right->tracks[0].clips[0].sourceOffset == 2000);
    CHECK (right->tracks[0].clips[0].length == 5000);

    const auto farLeft = songForDrag (song, drag, view, 0);
    REQUIRE (farLeft.has_value());
    CHECK (farLeft->tracks[0].clips[0].timelineStart == 0);

    CHECK (songForDrag (song, drag, view, 300) == song);
    CHECK (std::string (labelForDrag (ClipPart::body)) == "Move Clip");
}

TEST_CASE ("dragging a Clip's start edge trims its start, clamped to the source's first sample", "[ui][timeline]")
{
    const auto song = songWithClips ({ clip ("c", 10000, 5000, 2000) });
    const TimelineView view { .firstSample = 0, .samplesPerPixel = 100, .laneX = 0 };
    const ClipDrag drag { .clipId = "c", .part = ClipPart::startEdge, .downX = 100 };

    const auto inward = songForDrag (song, drag, view, 110);
    REQUIRE (inward.has_value());
    CHECK (inward->tracks[0].clips[0] == clip ("c", 11000, 4000, 3000));

    // Outward by 5000 samples: only 2000 source samples come before the Clip.
    const auto outward = songForDrag (song, drag, view, 50);
    REQUIRE (outward.has_value());
    CHECK (outward->tracks[0].clips[0] == clip ("c", 8000, 7000, 0));

    // Past the end: one sample is left.
    const auto past = songForDrag (song, drag, view, 400);
    REQUIRE (past.has_value());
    CHECK (past->tracks[0].clips[0] == clip ("c", 14999, 1, 6999));
    CHECK (std::string (labelForDrag (ClipPart::startEdge)) == "Trim Clip");
}

TEST_CASE ("dragging a Clip's end edge trims its end, clamped to the source's last sample", "[ui][timeline]")
{
    const auto song = songWithClips ({ clip ("c", 10000, 5000, 2000) }, 9000);
    const TimelineView view { .firstSample = 0, .samplesPerPixel = 100, .laneX = 0 };
    const ClipDrag drag { .clipId = "c", .part = ClipPart::endEdge, .downX = 150 };

    const auto shorter = songForDrag (song, drag, view, 130);
    REQUIRE (shorter.has_value());
    CHECK (shorter->tracks[0].clips[0] == clip ("c", 10000, 3000, 2000));

    // The Take has 9000 samples and the Clip reads from 2000: at most 7000 long.
    const auto longer = songForDrag (song, drag, view, 400);
    REQUIRE (longer.has_value());
    CHECK (longer->tracks[0].clips[0] == clip ("c", 10000, 7000, 2000));

    const auto collapsed = songForDrag (song, drag, view, 0);
    REQUIRE (collapsed.has_value());
    CHECK (collapsed->tracks[0].clips[0].length == 1);
    CHECK (std::string (labelForDrag (ClipPart::endEdge)) == "Trim Clip");
}

TEST_CASE ("a waveform column is the min and max of the peaks it covers, over every channel", "[ui][timeline]")
{
    Peaks peaks;
    peaks.lengthSamples = 4 * 256;
    peaks.channels = {
        { { -0.1f, 0.1f }, { -0.5f, 0.2f }, { -0.3f, 0.3f }, { -0.2f, 0.9f } },
        { { -0.2f, 0.0f }, { -0.1f, 0.6f }, { -0.1f, 0.1f }, { 0.0f, 0.0f } },
    };

    // 512 samples per column from source sample 0: blocks {0, 1} then {2, 3}.
    CHECK (waveformColumns (peaks, 0, 512, 3) == std::vector<PeakRange> { { -0.5f, 0.6f }, { -0.3f, 0.9f }, { 0.0f, 0.0f } });
    // 128 samples per column from 256: each block covers two columns.
    CHECK (waveformColumns (peaks, 256, 128, 2) == std::vector<PeakRange> { { -0.5f, 0.6f }, { -0.5f, 0.6f } });
}
