#include "engine/Engine.h"

#include "EngineFixture.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cmath>

// Clip gain and fades heard through the engine (issue #29, ADR 0005 null tests): the output nulls
// against the source times the envelope the spec defines, computed here independently of the
// engine, at block sizes that do not divide the fades and after seeks into a fade.
//
// The envelope (the one v1 fade curve, linear in amplitude), for sample k of a Clip of length L:
//   gain  = 10^(gainDb / 20)
//   in    = k / fadeIn            while k < fadeIn            (0 at the Clip's first sample)
//   out   = (L - 1 - k) / fadeOut while L - 1 - k < fadeOut   (0 at the Clip's last sample)
//   value = source * gain * in * out
using overdub::engine::Engine;
using overdub::engine::EngineConfig;
using namespace overdub::engine::testing;
namespace song = overdub::song;

namespace
{
constexpr int maxBlockSize = 256;

struct ClipShape
{
    song::SampleCount timelineStart = 0;
    song::SampleCount sourceOffset = 0;
    song::SampleCount length = 0;
    double gainDb = 0.0;
    song::SampleCount fadeIn = 0;
    song::SampleCount fadeOut = 0;
};

song::Clip clipShaped (const song::Take& take, const ClipShape& shape)
{
    auto clip = clipOf ("clip-a", take, shape.timelineStart, shape.sourceOffset, shape.length);
    clip.gainDb = shape.gainDb;
    clip.fadeInLength = shape.fadeIn;
    clip.fadeOutLength = shape.fadeOut;
    return clip;
}

// What Song samples [from, from + length) should sound like, per the envelope above.
std::vector<float> expectedOutput (const std::vector<float>& source, const ClipShape& shape, std::int64_t from, std::int64_t length)
{
    std::vector<float> expected ((std::size_t) length, 0.0f);
    const auto gain = std::pow (10.0, shape.gainDb / 20.0);
    for (std::int64_t i = 0; i < length; ++i)
    {
        const auto k = from + i - shape.timelineStart;
        if (k < 0 || k >= shape.length)
            continue;
        auto amplitude = gain;
        if (k < shape.fadeIn)
            amplitude *= (double) k / (double) shape.fadeIn;
        const auto fromEnd = shape.length - 1 - k;
        if (fromEnd < shape.fadeOut)
            amplitude *= (double) fromEnd / (double) shape.fadeOut;
        expected[(std::size_t) i] = (float) ((double) source[(std::size_t) (shape.sourceOffset + k)] * amplitude);
    }
    return expected;
}

std::vector<float> slice (const std::vector<float>& samples, std::int64_t from, std::int64_t to)
{
    return { samples.begin() + from, samples.begin() + to };
}
} // namespace

TEST_CASE ("a Clip's gain and fades null against the computed envelope across buffer boundaries", "[engine][clip_gain]")
{
    TempPackage package;
    auto songToPlay = song::makeEmptySong (testSampleRate);
    const auto source = sine (12000, 997.0, 0.8f);
    const auto take = addTake (songToPlay, package, "take-a", source);

    const auto blockSize = GENERATE (256, 100, 37);
    const auto gainDb = GENERATE (0.0, -6.0, 4.5);
    const ClipShape shape { .timelineStart = 1000, .sourceOffset = 500, .length = 9000, .gainDb = gainDb, .fadeIn = 1234, .fadeOut = 2001 };
    songToPlay.tracks.push_back (trackWith ("track-a", { clipShaped (take, shape) }));
    INFO ("block size " << blockSize << ", gain " << gainDb << " dB");

    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = maxBlockSize } };
    engine.loadSong (songToPlay, package.root);
    engine.play();
    DeviceDriver device { 2, blockSize };
    const auto rendered = device.render (engine, 11000 / blockSize + 1);

    const auto expected = expectedOutput (source, shape, 0, rendered.length());
    REQUIRE (! nonZeroSamples (expected).empty());
    CHECK (peakResidualDbfs (rendered.channels[0], expected) <= -120.0);
    CHECK (peakResidualDbfs (rendered.channels[1], expected) <= -120.0);
}

TEST_CASE ("after a seek into a fade the output nulls against the envelope from there", "[engine][clip_gain]")
{
    TempPackage package;
    auto songToPlay = song::makeEmptySong (testSampleRate);
    const auto source = sine (12000, 440.0, 0.9f);
    const auto take = addTake (songToPlay, package, "take-a", source);
    const ClipShape shape { .timelineStart = 2000, .sourceOffset = 0, .length = 8000, .gainDb = -3.0, .fadeIn = 3000, .fadeOut = 2500 };
    songToPlay.tracks.push_back (trackWith ("track-a", { clipShaped (take, shape) }));

    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = maxBlockSize } };
    engine.loadSong (songToPlay, package.root);
    constexpr int blockSize = 100;
    DeviceDriver device { 2, blockSize };

    SECTION ("into the fade-in, before playing")
    {
        const std::int64_t seekTo = 3456;
        engine.seek (seekTo);
        engine.play();
        const auto rendered = device.render (engine, 30);
        CHECK (peakResidualDbfs (rendered.channels[0], expectedOutput (source, shape, seekTo, rendered.length())) <= -120.0);
    }

    SECTION ("into the fade-out, while playing")
    {
        engine.play();
        const auto before = device.render (engine, 5);
        CHECK (peakResidualDbfs (before.channels[0], expectedOutput (source, shape, 0, before.length())) <= -120.0);

        const std::int64_t seekTo = 8765;
        engine.seek (seekTo);
        const auto after = device.render (engine, 15);
        CHECK (peakResidualDbfs (after.channels[0], expectedOutput (source, shape, seekTo, after.length())) <= -120.0);
        // The Clip ends at 10000: its last sample is silent, and so is everything after it.
        CHECK (nonZeroSamples (slice (after.channels[0], 10000 - 1 - seekTo, after.length())).empty());
    }
}
