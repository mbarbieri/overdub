#include "capture/VideoPlacement.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

using overdub::capture::fitAnchors;
using overdub::capture::placeVideoTake;
using overdub::capture::songSampleAtMovieTime;
using overdub::capture::songSampleHeardAt;
using overdub::engine::Anchor;
using overdub::song::SampleCount;

namespace
{
constexpr int sampleRate = 48000;

// One anchor per callback of `blockSize` samples from Song sample `firstSample`, for a device whose
// clock runs `ppm` parts per million fast against host time. Host times are rounded to whole ns,
// plus a deterministic jitter of up to `jitterNs` (CoreAudio's timestamps wobble by tens of µs).
std::vector<Anchor> anchorsFor (SampleCount firstSample, int blocks, int blockSize, double ppm, std::uint64_t firstHostNs, double jitterNs = 0.0)
{
    const auto nsPerSample = 1.0e9 / (sampleRate * (1.0 + ppm * 1.0e-6));
    std::mt19937 random { 7 };
    std::uniform_real_distribution<double> wobble (-jitterNs, jitterNs);
    std::vector<Anchor> anchors;
    for (int block = 0; block < blocks; ++block)
    {
        const auto offset = (double) block * blockSize * nsPerSample + (jitterNs > 0.0 ? wobble (random) : 0.0);
        anchors.push_back (Anchor { firstSample + (SampleCount) block * blockSize, firstHostNs + (std::uint64_t) std::llround (offset) });
    }
    return anchors;
}
} // namespace

TEST_CASE ("the anchor fit recovers injected drift within 0.1 ppm", "[capture][video][clock]")
{
    // Ten minutes of 512-sample callbacks, with ±40 µs of timestamp jitter.
    const auto blocks = 10 * 60 * sampleRate / 512;
    for (const auto ppm : { 0.0, -8.8, 25.0, -120.0 })
    {
        const auto fit = fitAnchors (anchorsFor (96'000, blocks, 512, ppm, 5'000'000'000ULL, 40'000.0), sampleRate);
        REQUIRE (fit.has_value());
        CHECK (std::abs (fit->driftPpm (sampleRate) - ppm) < 0.1);
        CHECK (fit->anchorCount == blocks);
    }
}

TEST_CASE ("an anchor fit needs at least one anchor; one anchor runs at the nominal rate", "[capture][video][clock]")
{
    CHECK_FALSE (fitAnchors ({}, sampleRate).has_value());

    const auto fit = fitAnchors ({ Anchor { 4800, 2'000'000'000ULL } }, sampleRate);
    REQUIRE (fit.has_value());
    CHECK (std::abs (fit->driftPpm (sampleRate)) < 1.0e-9);
    // 100 ms after the anchor's output IO time, with no output latency, Song sample 4800 + 4800 leaves the device.
    CHECK (std::llround (songSampleHeardAt (*fit, 2'100'000'000ULL, 0)) == 9600);
}

TEST_CASE ("the heard Song sample at a host time is exact on synthetic anchors", "[capture][video][clock]")
{
    // 480-sample callbacks at 48 kHz: exactly 10 ms apart, no drift, no jitter.
    const auto fit = fitAnchors (anchorsFor (48'000, 500, 480, 0.0, 3'000'000'000ULL), sampleRate);
    REQUIRE (fit.has_value());

    // Output IO time of Song sample 48000 is 3 s. 250 ms later sample 60000 is at the output IO;
    // with 240 samples (5 ms) of output latency, the sound heard then left the IO 240 samples ago.
    const auto heard = songSampleHeardAt (*fit, 3'250'000'000ULL, 240);
    CHECK (heard == Catch::Approx (60'000.0 - 240.0).margin (1.0e-6));
    // Before the first anchor (a frame from pre-roll's last moments): the same line, extrapolated.
    CHECK (songSampleHeardAt (*fit, 2'990'000'000ULL, 0) == Catch::Approx (47'520.0).margin (1.0e-6));
}

TEST_CASE ("a Video Take starts at the Song sample heard at its first frame, minus the camera calibration", "[capture][video][clock]")
{
    const auto fit = fitAnchors (anchorsFor (0, 1000, 480, 0.0, 1'000'000'000ULL), sampleRate);
    REQUIRE (fit.has_value());

    // First frame at host 1.5 s: Song sample 24000 is at the output IO, 24000 - 96 is heard.
    const auto placement = placeVideoTake (*fit, 1'500'000'000ULL, 96, 0, 1.0, sampleRate);
    CHECK (placement.startSample == 24'000 - 96);
    CHECK (placement.songSamplesPerMovieSecond == Catch::Approx ((double) sampleRate).epsilon (1.0e-12));

    // A camera that stamps frames 480 samples (10 ms) late: the frame was captured earlier.
    const auto calibrated = placeVideoTake (*fit, 1'500'000'000ULL, 96, 480, 1.0, sampleRate);
    CHECK (calibrated.startSample == 24'000 - 96 - 480);
}

TEST_CASE ("every frame maps to the Song sample heard at its host time, across a camera stall", "[capture][video][clock]")
{
    // A drifting device (-8.8 ppm) and a camera at 30 fps that stalls for 1.5 s after frame 300.
    const auto anchors = anchorsFor (0, 60 * sampleRate / 512, 512, -8.8, 1'000'000'000ULL);
    const auto fit = fitAnchors (anchors, sampleRate);
    REQUIRE (fit.has_value());

    const std::uint64_t firstFrameHostNs = 1'200'000'000ULL;
    std::vector<std::uint64_t> frameHostNs;
    for (int frame = 0; frame < 900; ++frame)
    {
        const auto stall = frame > 300 ? 1'500'000'000ULL : 0ULL;
        frameHostNs.push_back (firstFrameHostNs + (std::uint64_t) frame * 33'333'333ULL + stall);
    }

    constexpr int outputLatency = 512;
    const auto trueNsPerSample = 1.0e9 / (sampleRate * (1.0 - 8.8e-6));
    const auto placement = placeVideoTake (*fit, firstFrameHostNs, outputLatency, 0, 1.0, sampleRate);
    // The start is where the first frame is heard, rounded to the nearest sample.
    CHECK (placement.startSample == std::llround (songSampleHeardAt (*fit, firstFrameHostNs, outputLatency)));
    CHECK (std::abs (placement.driftPpm - (-8.8)) < 0.1);

    // Movie time is the frame's own timestamp (never a frame count), so the frames after the stall
    // land where they were heard: within one sample of the device's true clock over the minute.
    for (const auto hostNs : frameHostNs)
    {
        const auto movieSeconds = (double) (hostNs - firstFrameHostNs) / 1.0e9;
        const auto mapped = songSampleAtMovieTime (placement.startSample, placement.songSamplesPerMovieSecond, movieSeconds);
        const auto heard = (double) (hostNs - 1'000'000'000ULL) / trueNsPerSample - outputLatency;
        CHECK (std::abs (mapped - heard) <= 1.0);
    }
}

TEST_CASE ("a capture clock running against host time scales the drift ratio", "[capture][video][clock]")
{
    const auto fit = fitAnchors (anchorsFor (0, 1000, 480, 0.0, 1'000'000'000ULL), sampleRate);
    REQUIRE (fit.has_value());
    // A capture clock 10 ppm fast: one movie second is 1 / 1.00001 host seconds.
    const auto placement = placeVideoTake (*fit, 1'500'000'000ULL, 0, 0, 1.00001, sampleRate);
    CHECK (placement.songSamplesPerMovieSecond == Catch::Approx (sampleRate / 1.00001).epsilon (1.0e-12));
}

TEST_CASE ("a frame after a camera stall maps exactly to the Song sample heard at its host time", "[capture][video][clock]")
{
    // 480-sample callbacks, exactly 10 ms apart; a 30 fps camera whose frame 10 arrives after a
    // 1.5 s stall. Movie time is that frame's own PTS, never 10 frame intervals.
    const auto fit = fitAnchors (anchorsFor (0, 2000, 480, 0.0, 1'000'000'000ULL), sampleRate);
    REQUIRE (fit.has_value());
    const auto placement = placeVideoTake (*fit, 1'200'000'000ULL, 48, 0, 1.0, sampleRate);
    CHECK (placement.startSample == 9600 - 48);

    const auto afterStallSeconds = 10.0 * 0.025 + 1.5; // frames 25 ms apart, then the stall
    CHECK (songSampleAtMovieTime (placement.startSample, placement.songSamplesPerMovieSecond, afterStallSeconds) == Catch::Approx (9552.0 + 1.75 * 48'000).margin (1.0e-6));
}
