#include "ui/TranscribeGeometry.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <map>
#include <string>
#include <vector>

using namespace overdub;
using namespace overdub::ui;

// Transcribe mode's geometry (issue #225): zoom limits, the clamped view, the wheel, the overview's
// click and the heard mix's waveform, from synthetic Songs and peaks.
namespace
{
// Peaks of `blocks` 256-sample blocks, every one from -level to +level, on one channel.
song::Peaks constantPeaks (int blocks, float level)
{
    song::Peaks peaks;
    peaks.lengthSamples = (SampleCount) blocks * song::samplesPerPeak;
    peaks.channels.assign (1, std::vector<song::PeakRange> ((std::size_t) blocks, song::PeakRange { -level, level }));
    return peaks;
}

song::Clip clipOf (const std::string& sourceId, SampleCount start, SampleCount length, double gainDb = 0.0)
{
    song::Clip clip;
    clip.id = song::makeId();
    clip.source = song::ClipSource { song::ClipSourceKind::backing, sourceId };
    clip.timelineStart = start;
    clip.length = length;
    clip.gainDb = gainDb;
    return clip;
}

song::Track trackWith (std::vector<song::Clip> clips)
{
    song::Track track;
    track.id = song::makeId();
    track.clips = std::move (clips);
    return track;
}

struct PeaksBySource
{
    std::map<std::string, song::Peaks> bySource;
    song::Peaks none;

    PeaksLookup lookup() const
    {
        return [this] (const song::ClipSource& source) -> const song::Peaks& {
            const auto found = bySource.find (source.id);
            return found != bySource.end() ? found->second : none;
        };
    }
};

float maximumAt (const std::vector<song::PeakRange>& columns, std::size_t column)
{
    return columns.at (column).maximum;
}
} // namespace

TEST_CASE ("zooming in stops at about 2 s across the width, and out once the whole Song fits", "[ui][transcribe][geometry]")
{
    // 2 s at 48 kHz over 1000 px is 96 samples per pixel (6.58 levels): 128 is the nearest power of two.
    CHECK (transcribeMinSamplesPerPixel (48000, 1000) == 128);
    // Over 750 px it is exactly 128.
    CHECK (transcribeMinSamplesPerPixel (48000, 750) == 128);
    // Over 1500 px it is exactly 64.
    CHECK (transcribeMinSamplesPerPixel (48000, 1500) == 64);
    CHECK (transcribeMinSamplesPerPixel (48000, 0) >= 1);
    for (const auto width : { 400, 800, 1000, 1500 })
    {
        const auto spanSeconds = (double) transcribeMinSamplesPerPixel (48000, width) * width / 48000.0;
        CHECK (spanSeconds > finestTranscribeSeconds / 1.5);
        CHECK (spanSeconds < finestTranscribeSeconds * 1.5);
    }

    // 60 s at 48 kHz over 1000 px needs 2880 samples per pixel: 4096 fits it.
    CHECK (transcribeMaxSamplesPerPixel (60 * 48000, 1000, 128) == 4096);
    // A Song shorter than the finest span keeps the minimum.
    CHECK (transcribeMaxSamplesPerPixel (1000, 1000, 128) == 128);
}

TEST_CASE ("the view is clamped to the zoom limits and never runs past the Song's end", "[ui][transcribe][geometry]")
{
    const SampleCount length = 100000;
    CHECK (clampedTranscribeView ({ 500, 4, 0 }, 16, 256, length, 100) == TimelineView { 500, 16, 0 });
    CHECK (clampedTranscribeView ({ 500, 4096, 0 }, 16, 256, length, 100) == TimelineView { 500, 256, 0 });
    // Zoomed out past the Song's length, it starts at 0.
    CHECK (clampedTranscribeView ({ 500, 4096, 0 }, 16, 4096, length, 100) == TimelineView { 0, 4096, 0 });
    CHECK (clampedTranscribeView ({ -50, 64, 0 }, 16, 256, length, 100) == TimelineView { 0, 64, 0 });
    // 64 * 100 samples are visible: the last first sample is 100000 - 6400.
    CHECK (clampedTranscribeView ({ 99000, 64, 0 }, 16, 256, length, 100) == TimelineView { 93600, 64, 0 });
}

TEST_CASE ("the wheel scrolls time either way, and command-wheel up zooms in", "[ui][transcribe][geometry]")
{
    CHECK (transcribeWheelPixels (0.0f, -0.1f) == 40);
    CHECK (transcribeWheelPixels (0.0f, 0.1f) == -40);
    CHECK (transcribeWheelPixels (-0.1f, 0.0f) == 40);
    CHECK (transcribeWheelPixels (0.0f, 0.0f) == 0);

    CHECK (transcribeWheelZoomScale (0.0f) == Catch::Approx (1.0));
    CHECK (transcribeWheelZoomScale (0.125f) == Catch::Approx (2.0));
    CHECK (transcribeWheelZoomScale (-0.125f) == Catch::Approx (0.5));
}

TEST_CASE ("a click on the overview centres the visible window on the Song sample there", "[ui][transcribe][geometry]")
{
    // 1000 px span 100000 samples: x = 500 is sample 50000; a 10000-sample window starts at 45000.
    CHECK (overviewViewStart (500, 1000, 100000, 10000) == 45000);
    CHECK (overviewViewStart (10, 1000, 100000, 10000) == 0);
}

TEST_CASE ("the heard mix's waveform sums the Tracks that sound, with their gains, under the top Clip", "[ui][transcribe][geometry][mix]")
{
    PeaksBySource peaks;
    peaks.bySource["low"] = constantPeaks (40, 0.25f);
    peaks.bySource["high"] = constantPeaks (40, 0.5f);

    song::Song song;
    song.sampleRate = 48000;
    // Track 1: "low" over samples [0, 2560). Track 2: "high" over [1280, 3840).
    song.tracks.push_back (trackWith ({ clipOf ("low", 0, 2560) }));
    song.tracks.push_back (trackWith ({ clipOf ("high", 1280, 2560) }));

    // 256 samples per column, 16 columns: 0..4 only low, 5..9 both, 10..14 only high, 15 none.
    const auto mixed = heardMixColumns (song, peaks.lookup(), 0, 256, 16);
    REQUIRE (mixed.size() == 16);
    CHECK (maximumAt (mixed, 0) == Catch::Approx (0.25f));
    CHECK (mixed[0].minimum == Catch::Approx (-0.25f));
    CHECK (maximumAt (mixed, 6) == Catch::Approx (0.75f));
    CHECK (maximumAt (mixed, 12) == Catch::Approx (0.5f));
    CHECK (maximumAt (mixed, 15) == Catch::Approx (0.0f));

    SECTION ("a muted Track is not heard, and a soloed one is all that is heard")
    {
        auto muted = song;
        muted.tracks[1].mute = true;
        CHECK (maximumAt (heardMixColumns (muted, peaks.lookup(), 0, 256, 16), 6) == Catch::Approx (0.25f));
        auto soloed = song;
        soloed.tracks[1].solo = true;
        CHECK (maximumAt (heardMixColumns (soloed, peaks.lookup(), 0, 256, 16), 2) == Catch::Approx (0.0f));
        CHECK (maximumAt (heardMixColumns (soloed, peaks.lookup(), 0, 256, 16), 6) == Catch::Approx (0.5f));
    }

    SECTION ("Clip gain, Track volume and the Master scale it, and the sum is clamped to 1")
    {
        auto quieter = song;
        quieter.tracks[0].clips[0].gainDb = -6.0;
        CHECK (maximumAt (heardMixColumns (quieter, peaks.lookup(), 0, 256, 16), 0) == Catch::Approx (0.25f * 0.501187f).epsilon (1e-4));
        auto louder = song;
        louder.tracks[1].volumeDb = 12.0;
        CHECK (maximumAt (heardMixColumns (louder, peaks.lookup(), 0, 256, 16), 12) == Catch::Approx (1.0f));
        CHECK (heardMixColumns (louder, peaks.lookup(), 0, 256, 16)[12].minimum == Catch::Approx (-1.0f));
        auto master = song;
        master.master.volumeDb = -6.0;
        CHECK (maximumAt (heardMixColumns (master, peaks.lookup(), 0, 256, 16), 12) == Catch::Approx (0.5f * 0.501187f).epsilon (1e-4));
    }

    SECTION ("only the top Clip of a Track is heard where Clips overlap")
    {
        auto layered = song;
        layered.tracks.resize (1);
        layered.tracks[0].clips.push_back (clipOf ("high", 1280, 256)); // on top over column 5
        const auto columns = heardMixColumns (layered, peaks.lookup(), 0, 256, 16);
        CHECK (maximumAt (columns, 4) == Catch::Approx (0.25f));
        CHECK (maximumAt (columns, 5) == Catch::Approx (0.5f));
        CHECK (maximumAt (columns, 6) == Catch::Approx (0.25f));
    }

    SECTION ("the columns start at the view's first sample")
    {
        const auto columns = heardMixColumns (song, peaks.lookup(), 2560, 256, 6);
        CHECK (maximumAt (columns, 0) == Catch::Approx (0.5f));
        CHECK (maximumAt (columns, 5) == Catch::Approx (0.0f));
    }
}
