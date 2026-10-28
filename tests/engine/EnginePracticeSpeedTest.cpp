#include "engine/Engine.h"

#include "song/TransposeRender.h"

#include "../song/AudioMeasurements.h"
#include "EngineFixture.h"
#include "support/RealtimeGuardCatch.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <vector>

using namespace overdub::engine;
using namespace overdub::engine::testing;
using Catch::Matchers::WithinAbs;

// Practice Speed in the engine (issue #212, ADR 0009): a plan at a Practice Speed plays each source's
// render, and the timeline stays in Song time: the playhead advances at the speed times real time.
namespace
{
// A Song with one Take of `samples` in one Clip at `timelineStart`, from the Take's start.
overdub::song::Song songWithTake (TempPackage& package, const std::vector<float>& samples, overdub::song::SampleCount timelineStart = 0)
{
    auto song = overdub::song::makeEmptySong (testSampleRate);
    const auto take = addTake (song, package, "take-a", samples);
    song.tracks.push_back (trackWith ("track-a", { clipOf ("clip-a", take, timelineStart, 0, (overdub::song::SampleCount) samples.size()) }));
    return song;
}

PracticePlayback practiceWith (int speedPercent, const juce::File& render)
{
    return { .speedPercent = speedPercent, .renders = { { { overdub::song::ClipSourceKind::take, "take-a" }, render } } };
}

// The index of the sample with the largest magnitude.
std::int64_t peakIndex (const std::vector<float>& samples)
{
    std::int64_t peak = 0;
    for (std::size_t i = 1; i < samples.size(); ++i)
        if (std::abs (samples[i]) > std::abs (samples[(std::size_t) peak]))
            peak = (std::int64_t) i;
    return peak;
}
} // namespace

TEST_CASE ("at 80% a Clip plays its source's render, and an impulse at Song time t is heard after t / 0.8 of playback", "[engine][practice_speed]")
{
    TempPackage package;
    // Song time 800: the Clip starts at 400 and its Take's impulse is 400 in.
    const auto song = songWithTake (package, impulse (2000, 400), 400);
    // A stand-in render of the Take at 80% (2500 samples): source sample 400 at 400 / 0.8.
    const auto render = package.root.getChildFile ("practice/take-a.speed80.wav");
    writeFloatWav (render, { impulse (2500, 500) });

    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = 256 } };
    engine.loadSong (song, package.root, {}, std::nullopt, practiceWith (80, render));
    engine.play();
    DeviceDriver device { 2, 256 };
    const auto rendered = device.render (engine, 8);

    CHECK (nonZeroSamples (rendered.channels[0]) == std::vector<std::int64_t> { 1000 });
    // 2048 samples of playback move the playhead 2048 × 0.8 Song samples.
    CHECK (engine.position() == 1638);
}

TEST_CASE ("at a Practice Speed a source without a render plays silence", "[engine][practice_speed]")
{
    TempPackage package;
    const auto song = songWithTake (package, impulse (2000, 400));

    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = 256 } };
    engine.loadSong (song, package.root, {}, std::nullopt, PracticePlayback { .speedPercent = 80 });
    engine.play();
    DeviceDriver device { 2, 256 };

    CHECK (nonZeroSamples (device.render (engine, 12).channels[0]).empty());
}

TEST_CASE ("at 100% the renders are ignored and every source plays its own file", "[engine][practice_speed]")
{
    TempPackage package;
    const auto song = songWithTake (package, impulse (2000, 400));
    const auto render = package.root.getChildFile ("practice/take-a.speed80.wav");
    writeFloatWav (render, { impulse (2500, 500) });

    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = 256 } };
    engine.loadSong (song, package.root, {}, std::nullopt, practiceWith (100, render));
    engine.play();
    DeviceDriver device { 2, 256 };

    CHECK (nonZeroSamples (device.render (engine, 8).channels[0]) == std::vector<std::int64_t> { 400 });
}

TEST_CASE ("a plan at another Practice Speed loaded during play takes over at the same Song time", "[engine][practice_speed]")
{
    TempPackage package;
    const auto song = songWithTake (package, impulse (4000, 1600));
    const auto render = package.root.getChildFile ("practice/take-a.speed80.wav");
    writeFloatWav (render, { impulse (5000, 2000) });

    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = 256 } };
    engine.loadSong (song, package.root);
    engine.play();
    DeviceDriver device { 2, 256 };
    const auto atFullSpeed = device.render (engine, 4);
    REQUIRE (engine.position() == 1024);

    engine.loadSong (song, package.root, {}, std::nullopt, practiceWith (80, render));
    const auto atPracticeSpeed = device.render (engine, 8);

    CHECK (nonZeroSamples (atFullSpeed.channels[0]).empty());
    // Song time 1024 on: the impulse 576 Song samples later is heard 576 / 0.8 = 720 samples in.
    CHECK (nonZeroSamples (atPracticeSpeed.channels[0]) == std::vector<std::int64_t> { 720 });
    CHECK (engine.position() == 1024 + 1638);
}

TEST_CASE ("a seek at a Practice Speed is to a Song sample", "[engine][practice_speed]")
{
    TempPackage package;
    const auto song = songWithTake (package, impulse (4000, 1600));
    const auto render = package.root.getChildFile ("practice/take-a.speed80.wav");
    writeFloatWav (render, { impulse (5000, 2000) });

    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = 256 } };
    engine.loadSong (song, package.root, {}, std::nullopt, practiceWith (80, render));
    engine.seek (800);
    CHECK (engine.position() == 800);
    engine.play();
    DeviceDriver device { 2, 256 };
    const auto rendered = device.render (engine, 8);

    // 800 Song samples from the seek to the impulse: 1000 samples of playback.
    CHECK (nonZeroSamples (rendered.channels[0]) == std::vector<std::int64_t> { 1000 });
}

TEST_CASE ("at a Practice Speed a Clip's fade and gain apply in Song time", "[engine][practice_speed]")
{
    TempPackage package;
    auto song = songWithTake (package, std::vector<float> (4000, 1.0f));
    song.tracks[0].clips[0].fadeInLength = 1000;
    song.tracks[0].clips[0].gainDb = -6.0;
    const auto render = package.root.getChildFile ("practice/take-a.speed80.wav");
    writeFloatWav (render, { std::vector<float> (5000, 1.0f) });

    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = 256 } };
    engine.loadSong (song, package.root, {}, std::nullopt, practiceWith (80, render));
    engine.play();
    DeviceDriver device { 2, 256 };
    const auto left = device.render (engine, 8).channels[0];

    // The fade covers Song samples 0..1000, so 1250 samples of playback; then the gain alone.
    const auto gain = std::pow (10.0, -6.0 / 20.0) * centrePanGain;
    CHECK_THAT (left[625], WithinAbs (gain * 0.5, 1.0e-3));
    CHECK_THAT (left[1500], WithinAbs (gain, 1.0e-6));
}

TEST_CASE ("at 80% a rendered sine Backing Track keeps its frequency and an impulse lands at t / 0.8", "[engine][practice_speed]")
{
    TempPackage package;
    auto samples = sine (2 * testSampleRate, 440.0);
    samples[(std::size_t) testSampleRate] = 1.0f; // an impulse at Song time 1 s, over the sine
    const auto song = songWithTake (package, samples);
    const auto render = package.root.getChildFile ("take-a.speed80.wav");
    REQUIRE (overdub::song::renderAtPracticeSpeed (package.root.getChildFile ("audio/take-a.wav"), render, testSampleRate, 0, 80, {}).has_value());

    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = 256 } };
    engine.loadSong (song, package.root, {}, std::nullopt, practiceWith (80, render));
    engine.play();
    DeviceDriver device { 2, 256 };
    const auto left = device.render (engine, 2 * testSampleRate * 5 / 4 / 256).channels[0];

    // ADR 0005: within 5 cents, measured over the first second away from the impulse.
    const auto measured = overdub::song::testing::measuredFrequency (left.data(), 6000, 54000, testSampleRate);
    CHECK_THAT (1200.0 * std::log2 (measured / 440.0), WithinAbs (0.0, 5.0));
    // ADR 0005: within 1 ms of 1 s / 0.8.
    CHECK_THAT ((double) peakIndex (left), WithinAbs (60000.0, 48.0));
}

TEST_CASE ("playing at a Practice Speed neither allocates nor locks on the audio thread", "[engine][practice_speed][rt]")
{
    TempPackage package;
    const auto song = songWithTake (package, impulse (4000, 1600));
    const auto render = package.root.getChildFile ("practice/take-a.speed80.wav");
    writeFloatWav (render, { impulse (5000, 2000) });
    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = 256 } };
    engine.loadSong (song, package.root);
    engine.play();
    DeviceDriver device { 2, 256 };
    device.processOneBlock (engine);

    engine.loadSong (song, package.root, {}, std::nullopt, practiceWith (80, render));
    REQUIRE_REALTIME_SAFE (for (int block = 0; block < 4; ++block) device.processOneBlock (engine));
    engine.seek (900);
    REQUIRE_REALTIME_SAFE (for (int block = 0; block < 4; ++block) device.processOneBlock (engine));
}
