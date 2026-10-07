#include "engine/Engine.h"

#include "EngineFixture.h"
#include "TestProcessors.h"
#include "support/RealtimeGuardCatch.h"

#include <catch2/catch_test_macros.hpp>

using overdub::engine::Engine;
using overdub::engine::EngineConfig;
using namespace overdub::engine::testing;

namespace
{
// Two Tracks, one behind the test delay processor, so the guarded graph includes PDC delay lines.
overdub::song::Song twoTrackSong (const TempPackage& package, std::int64_t impulseAt)
{
    auto song = overdub::song::makeEmptySong (testSampleRate);
    const auto takeA = addTake (song, package, "take-a-" + std::to_string (impulseAt), impulse (8000, impulseAt));
    const auto takeB = addTake (song, package, "take-b-" + std::to_string (impulseAt), sine (8000, 220.0));
    song.tracks.push_back (trackWith ("track-a", { clipOf ("clip-a", takeA, 0, 0, 8000) }));
    song.tracks.push_back (trackWith ("track-b", { clipOf ("clip-b1", takeB, 0, 0, 4000), clipOf ("clip-b2", takeB, 2000, 100, 4000) }));
    return song;
}

overdub::engine::TrackInsertFactory delayOnTrackB()
{
    return insertOnTrack ("track-b", [] { return std::make_unique<DelayProcessor> (64); });
}
} // namespace

TEST_CASE ("process() neither allocates nor locks while playing, seeking and stopping", "[engine][rt]")
{
    TempPackage package;
    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = 256 } };
    engine.loadSong (twoTrackSong (package, 1000), package.root, delayOnTrackB());

    DeviceDriver device { 2, 256 };
    DeviceDriver smallBlocks { 2, 100 };

    // The first callback takes the plan, so it is guarded too.
    REQUIRE_REALTIME_SAFE (device.processOneBlock (engine));

    engine.play();
    REQUIRE_REALTIME_SAFE (for (int block = 0; block < 8; ++block) device.processOneBlock (engine));

    // A device may deliver fewer samples than the maximum block.
    REQUIRE_REALTIME_SAFE (for (int block = 0; block < 4; ++block) smallBlocks.processOneBlock (engine));

    engine.seek (500);
    REQUIRE_REALTIME_SAFE (device.processOneBlock (engine); device.processOneBlock (engine));

    engine.stop();
    REQUIRE_REALTIME_SAFE (device.processOneBlock (engine));
}

TEST_CASE ("swapping in a new Song's plan during playback neither allocates nor locks on the audio thread", "[engine][rt]")
{
    TempPackage package;
    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = 256 } };
    engine.loadSong (twoTrackSong (package, 3000), package.root, delayOnTrackB());
    engine.play();

    DeviceDriver device { 2, 256 };
    device.render (engine, 4);

    // Built here, off the audio thread; the swap itself happens inside the next process().
    engine.loadSong (twoTrackSong (package, 2000), package.root);
    REQUIRE_REALTIME_SAFE (for (int block = 0; block < 4; ++block) device.processOneBlock (engine));

    // The new plan has no delay processor, so its latency is 0, and it plays the new Song.
    CHECK (engine.latencySamples() == 0);

    // A second swap only happens after the replaced plan is freed off the audio thread.
    engine.housekeeping (overdub::core::hostTimeNowNs());
    engine.loadSong (twoTrackSong (package, 3000), package.root, delayOnTrackB());
    REQUIRE_REALTIME_SAFE (device.processOneBlock (engine));
    CHECK (engine.latencySamples() == 64);
}

TEST_CASE ("after a swap the new Song plays from the current position", "[engine][transport]")
{
    TempPackage package;
    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = 256 } };

    auto first = overdub::song::makeEmptySong (testSampleRate);
    const auto firstTake = addTake (first, package, "take-first", impulse (4000, 1500));
    first.tracks.push_back (trackWith ("track", { clipOf ("clip", firstTake, 0, 0, 4000) }));

    auto second = overdub::song::makeEmptySong (testSampleRate);
    const auto secondTake = addTake (second, package, "take-second", impulse (4000, 1800));
    second.tracks.push_back (trackWith ("track", { clipOf ("clip", secondTake, 0, 0, 4000) }));

    engine.loadSong (first, package.root);
    engine.play();
    DeviceDriver device { 2, 256 };
    const auto before = device.render (engine, 4); // Song samples 0..1023
    CHECK (nonZeroSamples (before.channels[0]).empty());

    engine.loadSong (second, package.root);
    const auto after = device.render (engine, 4); // Song samples 1024..2047

    // Only the second Song's impulse, at Song sample 1800.
    CHECK (nonZeroSamples (after.channels[0]) == std::vector<std::int64_t> { 1800 - 1024 });
}
