#include "capture/ClapMeasurement.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <vector>

using overdub::capture::ClapPlacement;
using overdub::capture::findClaps;
using overdub::capture::measureClaps;
using overdub::capture::MovieFrame;
using overdub::capture::visualClapSeconds;
using overdub::song::SampleCount;

namespace
{
constexpr int sampleRate = 48000;

// `count` frames at 30 fps from movie time 0, still, except the hands closing over the frames
// ending at `lastClosingFrame` (motion rising to it) and stopping after it.
std::vector<MovieFrame> framesWithClapEndingAt (int count, std::vector<int> lastClosingFrames)
{
    std::vector<MovieFrame> frames;
    for (int i = 0; i < count; ++i)
        frames.push_back (MovieFrame { i / 30.0, 0.5 });
    for (const auto last : lastClosingFrames)
    {
        frames[(std::size_t) last - 3].motion = 10.0;
        frames[(std::size_t) last - 2].motion = 25.0;
        frames[(std::size_t) last - 1].motion = 40.0;
        frames[(std::size_t) last].motion = 60.0;
        frames[(std::size_t) last + 1].motion = 3.0; // the hands have met
    }
    return frames;
}
} // namespace

TEST_CASE ("claps are found at their onsets, at least 250 ms apart", "[capture][video][clap]")
{
    std::vector<float> samples ((std::size_t) sampleRate * 2, 0.0f);
    for (std::size_t i = 0; i < samples.size(); i += 97)
        samples[i] = 0.05f; // background below 30% of the peak
    for (const SampleCount onset : { 4800, 24'000, 60'000 })
        for (SampleCount i = 0; i < 200; ++i)
            samples[(std::size_t) (onset + i)] = 0.8f * (i % 2 == 0 ? 1.0f : -1.0f);
    samples[24'100 + 400] = 0.9f; // the same clap's ringing, inside the 250 ms after it

    CHECK (findClaps (samples, sampleRate) == std::vector<SampleCount> { 4800, 24'000, 60'000 });
}

TEST_CASE ("the visual clap is the midpoint of the last closing frame and the one before it", "[capture][video][clap]")
{
    auto frames = framesWithClapEndingAt (120, { 43 });
    frames[100].motion = 90.0; // someone walks by, outside the window

    const auto seen = visualClapSeconds (frames, 1.4, 0.3);
    REQUIRE (seen.has_value());
    CHECK (*seen == Catch::Approx ((42.0 + 43.0) / 2.0 / 30.0));

    CHECK_FALSE (visualClapSeconds (framesWithClapEndingAt (120, {}), 1.4, 0.3).has_value());
}

TEST_CASE ("a clap's offset is the visual clap minus the audio clap, through the Video Take's placement", "[capture][video][clap]")
{
    // The Take starts at Song sample 1000, the Video Take at 2000, at exactly 48000 samples per
    // movie second. Clap 1 is heard at Song sample 50240 (movie 1.005 s); its hands meet between
    // frames 30 and 31 (1.0167 s): 11.7 ms late. Clap 2 at movie 3.005 s is seen 5 frames late.
    std::vector<float> take ((std::size_t) sampleRate * 5, 0.0f);
    take[50'240 - 1000] = 1.0f;
    take[(std::size_t) (2000 + 3.005 * sampleRate) - 1000] = 1.0f;
    const ClapPlacement placement { .sampleRate = sampleRate, .takeOrigin = 1000, .videoStartSample = 2000, .songSamplesPerMovieSecond = sampleRate };

    const auto report = measureClaps (take, placement, framesWithClapEndingAt (150, { 31, 96 }));
    REQUIRE (report.claps.size() == 2);
    CHECK (report.claps[0].songSample == 50'240);
    CHECK (report.claps[0].audioMovieSeconds == Catch::Approx (1.005));
    REQUIRE (report.claps[0].offsetMs.has_value());
    CHECK (*report.claps[0].offsetMs == Catch::Approx ((30.5 / 30.0 - 1.005) * 1000.0));
    REQUIRE (report.claps[1].offsetMs.has_value());
    CHECK (*report.claps[1].offsetMs == Catch::Approx ((95.5 / 30.0 - 3.005) * 1000.0));
    CHECK (report.frameIntervalMs == Catch::Approx (1000.0 / 30.0));
    CHECK_FALSE (report.pass); // clap 2 is 5 frames off

    const auto onTime = measureClaps (take, placement, framesWithClapEndingAt (150, { 31, 91 }));
    CHECK (onTime.pass);
    REQUIRE (onTime.meanOffsetMs.has_value());

    const auto fields = overdub::capture::clapReportFields (onTime);
    CHECK (fields["pass"] == true);
    REQUIRE (fields["claps"].size() == 2);
    CHECK (fields["claps"][0]["song_sample"] == 50'240);
    CHECK (fields["claps"][0].contains ("offset_ms"));
}

TEST_CASE ("a clap outside the movie, or a movie with no claps, does not pass", "[capture][video][clap]")
{
    std::vector<float> take ((std::size_t) sampleRate * 5, 0.0f);
    take[100] = 1.0f; // heard before the movie starts
    const ClapPlacement placement { .sampleRate = sampleRate, .takeOrigin = 0, .videoStartSample = 24'000, .songSamplesPerMovieSecond = sampleRate };

    const auto report = measureClaps (take, placement, framesWithClapEndingAt (60, {}));
    REQUIRE (report.claps.size() == 1);
    CHECK_FALSE (report.claps[0].offsetMs.has_value());
    CHECK_FALSE (report.pass);
    CHECK_FALSE (report.meanOffsetMs.has_value());
}
