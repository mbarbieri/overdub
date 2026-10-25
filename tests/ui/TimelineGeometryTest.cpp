#include "ui/TimelineGeometry.h"

#include "ui/SongEdits.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cmath>

using namespace overdub::ui;
using overdub::song::Clip;
using overdub::song::ClipSourceKind;
using overdub::song::PeakRange;
using overdub::song::Peaks;
using overdub::song::SampleCount;
using overdub::song::Song;

namespace
{
std::string dragLabel (const Song& song, const ClipDrag& drag, const TimelineView& view)
{
    const auto edit = intentForDrag (song, drag, view, drag.downX, drag.downY);
    REQUIRE (edit.has_value());
    const auto edited = editedSong (song, *edit);
    REQUIRE (edited.has_value());
    return edited->label;
}

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

TEST_CASE ("a pinch zooms by whole levels around the pointer, once it reaches half a level", "[ui][timeline][pinch]")
{
    // The sample under x = 150 is 60000 + 50 * 512 = 85600.
    const TimelineView view { .firstSample = 60000, .samplesPerPixel = 512, .laneX = 100 };

    const auto spread = pinchZoomed (view, 0.0, 2.0f, 150);
    CHECK (spread.view.samplesPerPixel == 256);
    CHECK (spread.view.firstSample == 85600 - 50 * 256);
    CHECK (sampleForX (spread.view, 150) == 85600);
    CHECK (spread.pendingLevels == 0.0);

    const auto closed = pinchZoomed (view, 0.0, 0.5f, 150);
    CHECK (closed.view.samplesPerPixel == 1024);
    CHECK (sampleForX (closed.view, 150) == 85600);

    // A third of a level waits; a second third takes the level, and pinching back returns.
    const auto third = std::exp2 (1.0 / 3.0);
    const auto small = pinchZoomed (view, 0.0, (float) third, 150);
    CHECK (small.view == view);
    const auto twoThirds = pinchZoomed (small.view, small.pendingLevels, (float) third, 150);
    CHECK (twoThirds.view.samplesPerPixel == 256);
    const auto back = pinchZoomed (twoThirds.view, twoThirds.pendingLevels, (float) (1.0 / third / third), 150);
    CHECK (back.view.samplesPerPixel == 512);

    // Left of the lanes, the anchor is laneX; zooming out never scrolls before the Song's start.
    CHECK (pinchZoomed (view, 0.0, 2.0f, 20).view.firstSample == 60000);
    CHECK (pinchZoomed (view, 0.0, 0.25f, 150).view.firstSample == 0);
}

TEST_CASE ("a pinch past a zoom limit builds up nothing to pinch back", "[ui][timeline][pinch]")
{
    const TimelineView tightest { .firstSample = 0, .samplesPerPixel = minSamplesPerPixel, .laneX = 0 };
    const auto further = pinchZoomed (tightest, 0.0, 16.0f, 0);
    CHECK (further.view.samplesPerPixel == minSamplesPerPixel);
    CHECK (further.pendingLevels == 0.0);
    CHECK (pinchZoomed (further.view, further.pendingLevels, 0.7f, 0).view.samplesPerPixel == minSamplesPerPixel * 2);

    const TimelineView widest { .firstSample = 0, .samplesPerPixel = maxSamplesPerPixel, .laneX = 0 };
    const auto wider = pinchZoomed (widest, 0.0, 1.0f / 16.0f, 0);
    CHECK (wider.view.samplesPerPixel == maxSamplesPerPixel);
    CHECK (pinchZoomed (wider.view, wider.pendingLevels, 1.5f, 0).view.samplesPerPixel == maxSamplesPerPixel / 2);
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

    const auto right = songForDrag (song, drag, view, 340, 0);
    REQUIRE (right.has_value());
    CHECK (right->tracks[0].clips[0].timelineStart == 10000 + 40 * 256);
    CHECK (right->tracks[0].clips[0].sourceOffset == 2000);
    CHECK (right->tracks[0].clips[0].length == 5000);

    const auto farLeft = songForDrag (song, drag, view, 0, 0);
    REQUIRE (farLeft.has_value());
    CHECK (farLeft->tracks[0].clips[0].timelineStart == 0);

    CHECK (songForDrag (song, drag, view, 300, 0) == song);
    CHECK (dragLabel (song, drag, view) == "Move Clip");
}

TEST_CASE ("dragging a Clip's start edge trims its start, clamped to the source's first sample", "[ui][timeline]")
{
    const auto song = songWithClips ({ clip ("c", 10000, 5000, 2000) });
    const TimelineView view { .firstSample = 0, .samplesPerPixel = 100, .laneX = 0 };
    const ClipDrag drag { .clipId = "c", .part = ClipPart::startEdge, .downX = 100 };

    const auto inward = songForDrag (song, drag, view, 110, 0);
    REQUIRE (inward.has_value());
    CHECK (inward->tracks[0].clips[0] == clip ("c", 11000, 4000, 3000));

    // Outward by 5000 samples: only 2000 source samples come before the Clip.
    const auto outward = songForDrag (song, drag, view, 50, 0);
    REQUIRE (outward.has_value());
    CHECK (outward->tracks[0].clips[0] == clip ("c", 8000, 7000, 0));

    // Past the end: one sample is left.
    const auto past = songForDrag (song, drag, view, 400, 0);
    REQUIRE (past.has_value());
    CHECK (past->tracks[0].clips[0] == clip ("c", 14999, 1, 6999));
    CHECK (dragLabel (song, drag, view) == "Trim Clip");
}

TEST_CASE ("dragging a Clip's end edge trims its end, clamped to the source's last sample", "[ui][timeline]")
{
    const auto song = songWithClips ({ clip ("c", 10000, 5000, 2000) }, 9000);
    const TimelineView view { .firstSample = 0, .samplesPerPixel = 100, .laneX = 0 };
    const ClipDrag drag { .clipId = "c", .part = ClipPart::endEdge, .downX = 150 };

    const auto shorter = songForDrag (song, drag, view, 130, 0);
    REQUIRE (shorter.has_value());
    CHECK (shorter->tracks[0].clips[0] == clip ("c", 10000, 3000, 2000));

    // The Take has 9000 samples and the Clip reads from 2000: at most 7000 long.
    const auto longer = songForDrag (song, drag, view, 400, 0);
    REQUIRE (longer.has_value());
    CHECK (longer->tracks[0].clips[0] == clip ("c", 10000, 7000, 2000));

    const auto collapsed = songForDrag (song, drag, view, 0, 0);
    REQUIRE (collapsed.has_value());
    CHECK (collapsed->tracks[0].clips[0].length == 1);
    CHECK (dragLabel (song, drag, view) == "Trim Clip");
}

TEST_CASE ("a Clip's fade handles sit at its fade ends and its gain tag in its top-right corner, under them", "[ui][timeline][clip_gain]")
{
    // 100 samples per pixel from x = 0: the Clip spans x [10, 110), its top at y = 40.
    const TimelineView view { .firstSample = 0, .samplesPerPixel = 100, .laneX = 0 };
    auto faded = clip ("c", 1000, 10000);
    faded.fadeInLength = 2000;  // ends at x = 30
    faded.fadeOutLength = 3000; // starts at x = 80

    const auto handles = clipHandleBounds (view, faded, 40);
    CHECK (handles.fadeIn == PixelRect { 30 - fadeHandleSize / 2, 40, fadeHandleSize, fadeHandleSize });
    CHECK (handles.fadeOut == PixelRect { 80 - fadeHandleSize / 2, 40, fadeHandleSize, fadeHandleSize });
    CHECK (handles.gainTag == PixelRect { 110 - clipEdgeGrabWidth - 2 - gainTagWidth, 40 + fadeHandleSize + 1, gainTagWidth, gainTagHeight });

    // No fades: the handles sit in the Clip's top corners, inside it.
    const auto plain = clipHandleBounds (view, clip ("c", 1000, 10000), 40);
    CHECK (plain.fadeIn.x == 10);
    CHECK (plain.fadeOut.x == 110 - fadeHandleSize);

    // A Clip too narrow for its gain tag has none.
    CHECK (clipHandleBounds (view, clip ("c", 1000, 3000), 40).gainTag.isEmpty());
}

TEST_CASE ("hit-testing a Clip's handles finds the fade handles first, then the gain tag", "[ui][timeline][clip_gain]")
{
    const TimelineView view { .firstSample = 0, .samplesPerPixel = 100, .laneX = 0 };
    auto faded = clip ("c", 1000, 10000);
    faded.fadeInLength = 2000;
    faded.fadeOutLength = 3000;
    const auto handles = clipHandleBounds (view, faded, 40);

    CHECK (hitTestClipHandles (view, faded, 40, 30, 43) == ClipPart::fadeInHandle);
    CHECK (hitTestClipHandles (view, faded, 40, 80, 40) == ClipPart::fadeOutHandle);
    CHECK (hitTestClipHandles (view, faded, 40, handles.gainTag.x + 2, handles.gainTag.y + 2) == ClipPart::gainTag);
    CHECK (! hitTestClipHandles (view, faded, 40, 30, 40 + fadeHandleSize).has_value());
    CHECK (! hitTestClipHandles (view, faded, 40, 55, 60).has_value());
}

TEST_CASE ("a fade handle never covers the gain tag, whatever the fade", "[ui][timeline][clip_gain]")
{
    const TimelineView view { .firstSample = 0, .samplesPerPixel = 100, .laneX = 0 };
    for (const SampleCount fadeOut : { SampleCount { 0 }, SampleCount { 500 }, SampleCount { 1500 }, SampleCount { 3000 }, SampleCount { 10000 } })
    {
        INFO ("fade-out " << fadeOut);
        auto faded = clip ("c", 1000, 10000);
        faded.fadeOutLength = fadeOut;
        const auto handles = clipHandleBounds (view, faded, 40);
        const auto tagCentreX = handles.gainTag.x + handles.gainTag.width / 2;
        const auto tagCentreY = handles.gainTag.y + handles.gainTag.height / 2;
        CHECK (hitTestClipHandles (view, faded, 40, tagCentreX, tagCentreY) == ClipPart::gainTag);
        CHECK (handles.fadeOut.y + handles.fadeOut.height <= handles.gainTag.y);
    }
}

TEST_CASE ("dragging a fade handle sets that fade by the dragged pixels, clamped into the Clip", "[ui][timeline][clip_gain]")
{
    auto faded = clip ("c", 10000, 5000);
    faded.fadeInLength = 1000;
    faded.fadeOutLength = 1500;
    const auto song = songWithClips ({ faded });
    const TimelineView view { .firstSample = 0, .samplesPerPixel = 100, .laneX = 0 };

    const ClipDrag fadeIn { .clipId = "c", .part = ClipPart::fadeInHandle, .downX = 110, .downY = 40 };
    CHECK (songForDrag (song, fadeIn, view, 115, 70)->tracks[0].clips[0].fadeInLength == 1500);
    CHECK (songForDrag (song, fadeIn, view, 100, 40)->tracks[0].clips[0].fadeInLength == 0);
    // Fade-in + fade-out never exceed the length: at most 5000 - 1500.
    CHECK (songForDrag (song, fadeIn, view, 400, 40)->tracks[0].clips[0].fadeInLength == 3500);
    CHECK (songForDrag (song, fadeIn, view, 400, 40)->tracks[0].clips[0].fadeOutLength == 1500);
    CHECK (dragLabel (song, fadeIn, view) == "Fade In");

    // Dragging the fade-out handle left lengthens the fade-out.
    const ClipDrag fadeOut { .clipId = "c", .part = ClipPart::fadeOutHandle, .downX = 135, .downY = 40 };
    CHECK (songForDrag (song, fadeOut, view, 125, 40)->tracks[0].clips[0].fadeOutLength == 2500);
    CHECK (songForDrag (song, fadeOut, view, 200, 40)->tracks[0].clips[0].fadeOutLength == 0);
    CHECK (songForDrag (song, fadeOut, view, 0, 40)->tracks[0].clips[0].fadeOutLength == 4000);
    CHECK (dragLabel (song, fadeOut, view) == "Fade Out");

    CHECK (songForDrag (song, fadeOut, view, 135, 90) == song);
}

TEST_CASE ("dragging the gain tag up raises the gain by 0.1 dB per pixel, within the limits", "[ui][timeline][clip_gain]")
{
    auto louder = clip ("c", 10000, 5000);
    louder.gainDb = -1.5;
    const auto song = songWithClips ({ louder });
    const TimelineView view { .firstSample = 0, .samplesPerPixel = 100, .laneX = 0 };
    const ClipDrag drag { .clipId = "c", .part = ClipPart::gainTag, .downX = 120, .downY = 50 };

    const auto gainAt = [&] (int x, int y) { return songForDrag (song, drag, view, x, y)->tracks[0].clips[0].gainDb; };
    CHECK (std::abs (gainAt (120, 20) - 1.5) < 1e-9);  // 30 pixels up: +3 dB
    CHECK (std::abs (gainAt (300, 65) + 3.0) < 1e-9);  // 15 pixels down; x is ignored
    CHECK (std::abs (gainAt (120, -500) - 12.0) < 1e-9);
    CHECK (std::abs (gainAt (120, 900) + 24.0) < 1e-9);
    CHECK (songForDrag (song, drag, view, 160, 50) == song);
    CHECK (dragLabel (song, drag, view) == "Clip Gain");
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

TEST_CASE ("a vertical wheel or swipe scrolls Tracks, a horizontal one or shift with the wheel scrolls time", "[ui][timeline][vertical_scroll]")
{
    // JUCE's deltas: positive deltaY is a wheel turned away (content moves down: scroll up).
    const auto vertical = wheelScroll (0.0f, -0.1f, false);
    CHECK (vertical.timePixels == 0);
    CHECK (vertical.verticalPixels == 40);

    const auto horizontal = wheelScroll (-0.1f, 0.0f, false);
    CHECK (horizontal.timePixels == 40);
    CHECK (horizontal.verticalPixels == 0);

    const auto shifted = wheelScroll (0.0f, -0.1f, true);
    CHECK (shifted.timePixels == 40);
    CHECK (shifted.verticalPixels == 0);

    // A trackpad swipe with both: each axis scrolls its own way.
    const auto diagonal = wheelScroll (0.05f, -0.1f, false);
    CHECK (diagonal.timePixels == -20);
    CHECK (diagonal.verticalPixels == 40);
}
