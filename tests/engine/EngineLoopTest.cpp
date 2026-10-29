#include "engine/Engine.h"

#include "EngineFixture.h"
#include "TestProcessors.h"
#include "support/RealtimeGuardCatch.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace overdub::engine;
using namespace overdub::engine::testing;

// Loops in the engine (issue #228, ADR 0010): while a Loop range is set, playback that reaches its
// end continues from its start, sample for sample, with no gap.
namespace
{
// One Track with one Take of `samples` in one Clip at Song sample 0.
overdub::song::Song songWithTake (TempPackage& package, const std::vector<float>& samples)
{
    auto song = overdub::song::makeEmptySong (testSampleRate);
    const auto take = addTake (song, package, "take-a", samples);
    song.tracks.push_back (trackWith ("track-a", { clipOf ("clip-a", take, 0, 0, (overdub::song::SampleCount) samples.size()) }));
    return song;
}

// Every `period` samples from `first`, below `length`.
std::vector<std::int64_t> everyPeriod (std::int64_t first, std::int64_t period, std::int64_t length)
{
    std::vector<std::int64_t> found;
    for (auto at = first; at < length; at += period)
        found.push_back (at);
    return found;
}
} // namespace

TEST_CASE ("with a Loop set, an impulse at its start is heard once per Loop length, to the sample", "[engine][loop]")
{
    TempPackage package;
    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = 256 } };
    engine.loadSong (songWithTake (package, impulse (8000, 1000)), package.root);
    engine.setLoop (LoopRange { .start = 1000, .end = 1700 });
    engine.seek (1000);
    engine.play();

    DeviceDriver device { 2, 256 };
    const auto rendered = device.render (engine, 16);

    CHECK (nonZeroSamples (rendered.channels[0]) == everyPeriod (0, 700, rendered.length()));
    // 4096 samples played: 5 whole Loops and 596 samples into the sixth.
    CHECK (engine.position() == 1000 + 4096 % 700);
}

TEST_CASE ("the Loop wraps after the graph's latency: playback from before the Loop reaches its end and goes on from its start", "[engine][loop]")
{
    TempPackage package;
    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = 256 } };
    engine.loadSong (songWithTake (package, impulse (8000, 1000)), package.root, insertOnTrack ("track-a", [] { return std::make_unique<DelayProcessor> (300); }));
    engine.setLoop (LoopRange { .start = 1000, .end = 1700 });
    engine.seek (600);
    engine.play();

    DeviceDriver device { 2, 256 };
    const auto rendered = device.render (engine, 16);

    // Song sample 1000 is heard 400 samples in (after the 300 stale ones), then once per Loop length.
    CHECK (nonZeroSamples (rendered.channels[0]) == everyPeriod (400, 700, rendered.length()));
}

TEST_CASE ("a Loop shorter than a block wraps several times within it", "[engine][loop]")
{
    TempPackage package;
    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = 512 } };
    engine.loadSong (songWithTake (package, impulse (8000, 2000)), package.root, insertOnTrack ("track-a", [] { return std::make_unique<DelayProcessor> (64); }));
    engine.setLoop (LoopRange { .start = 2000, .end = 2100 });
    engine.seek (1900);
    engine.play();

    DeviceDriver device { 2, 512 };
    const auto rendered = device.render (engine, 4);

    CHECK (nonZeroSamples (rendered.channels[0]) == everyPeriod (100, 100, rendered.length()));
    CHECK (engine.position() >= 2000);
    CHECK (engine.position() < 2100);
}

TEST_CASE ("playback past a Loop's end plays straight on, and so does playback with no Loop", "[engine][loop]")
{
    TempPackage package;
    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = 256 } };
    auto samples = impulse (8000, 500);
    samples[3000] = 1.0f;
    engine.loadSong (songWithTake (package, samples), package.root);
    DeviceDriver device { 2, 256 };

    SECTION ("past the end")
    {
        engine.setLoop (LoopRange { .start = 100, .end = 1000 });
        engine.seek (2000);
        engine.play();
        CHECK (nonZeroSamples (device.render (engine, 8).channels[0]) == std::vector<std::int64_t> { 1000 });
        CHECK (engine.position() == 2000 + 2048);
    }

    SECTION ("the Loop cleared")
    {
        engine.setLoop (LoopRange { .start = 0, .end = 1000 });
        engine.setLoop (std::nullopt);
        engine.play();
        CHECK (nonZeroSamples (device.render (engine, 16).channels[0]) == std::vector<std::int64_t> { 500, 3000 });
    }
}

TEST_CASE ("at a Practice Speed the Loop wraps at its Song range, in plan time", "[engine][loop][practice_speed]")
{
    TempPackage package;
    auto song = songWithTake (package, impulse (4000, 800));
    // A stand-in render of the Take at 80%: source sample 800 at 1000.
    const auto render = package.root.getChildFile ("practice/take-a.speed80.wav");
    writeFloatWav (render, { impulse (5000, 1000) });

    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = 256 } };
    engine.loadSong (song, package.root, {}, std::nullopt, PracticePlayback { .speedPercent = 80, .renders = { { { overdub::song::ClipSourceKind::take, "take-a" }, render } } });
    // Song 800..1600 is plan 1000..2000: 1000 samples of playback per Loop.
    engine.setLoop (LoopRange { .start = 800, .end = 1600 });
    engine.seek (800);
    engine.play();

    DeviceDriver device { 2, 256 };
    const auto rendered = device.render (engine, 16);

    CHECK (nonZeroSamples (rendered.channels[0]) == everyPeriod (0, 1000, rendered.length()));
}

TEST_CASE ("the Loop's wrap neither allocates nor locks on the audio thread", "[engine][loop][rt]")
{
    TempPackage package;
    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = 256 } };
    engine.loadSong (songWithTake (package, impulse (8000, 1000)), package.root, insertOnTrack ("track-a", [] { return std::make_unique<DelayProcessor> (64); }));
    DeviceDriver device { 2, 256 };
    device.processOneBlock (engine);

    engine.setLoop (LoopRange { .start = 1000, .end = 1150 });
    engine.seek (900);
    engine.play();
    REQUIRE_REALTIME_SAFE (for (int block = 0; block < 12; ++block) device.processOneBlock (engine));

    engine.setLoop (LoopRange { .start = 500, .end = 1500 });
    REQUIRE_REALTIME_SAFE (for (int block = 0; block < 12; ++block) device.processOneBlock (engine));

    engine.setLoop (std::nullopt);
    REQUIRE_REALTIME_SAFE (device.processOneBlock (engine));
}
