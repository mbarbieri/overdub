#include "engine/Engine.h"

#include "EngineFixture.h"
#include "TestProcessors.h"
#include "support/RealtimeGuardCatch.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using overdub::engine::Engine;
using overdub::engine::EngineConfig;
using namespace overdub::engine::testing;

namespace
{
// Two Tracks, one behind the test delay processor, so the guarded graph includes PDC delay lines.
// Track B's Clips have gain and fades, so the guarded render runs the Clip envelope too.
overdub::song::Song twoTrackSong (const TempPackage& package, std::int64_t impulseAt)
{
    auto song = overdub::song::makeEmptySong (testSampleRate);
    const auto takeA = addTake (song, package, "take-a-" + std::to_string (impulseAt), impulse (8000, impulseAt));
    const auto takeB = addTake (song, package, "take-b-" + std::to_string (impulseAt), sine (8000, 220.0));
    song.tracks.push_back (trackWith ("track-a", { clipOf ("clip-a", takeA, 0, 0, 8000) }));
    auto clipB1 = clipOf ("clip-b1", takeB, 0, 0, 4000);
    clipB1.gainDb = -4.0;
    clipB1.fadeInLength = 300;
    auto clipB2 = clipOf ("clip-b2", takeB, 2000, 100, 4000);
    clipB2.fadeInLength = 700;
    clipB2.fadeOutLength = 900;
    song.tracks.push_back (trackWith ("track-b", { clipB1, clipB2 }));
    return song;
}

overdub::engine::InsertFactory delayOnTrackB()
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

    // A second swap, with no housekeeping in between, is just as realtime safe.
    engine.loadSong (twoTrackSong (package, 3000), package.root, delayOnTrackB());
    REQUIRE_REALTIME_SAFE (device.processOneBlock (engine));
    CHECK (engine.latencySamples() == 64);
}

TEST_CASE ("every loaded Song plays from the next callback without housekeeping", "[engine][transport]")
{
    TempPackage package;
    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = 256 } };
    const auto song = twoTrackSong (package, 1000);
    DeviceDriver device { 2, 256 };
    engine.play();

    std::vector<int> latencies;
    for (int load = 0; load < 6; ++load)
    {
        engine.loadSong (song, package.root, load % 2 == 0 ? overdub::engine::InsertFactory {} : delayOnTrackB());
        device.processOneBlock (engine);
        latencies.push_back (engine.latencySamples());
    }

    CHECK (latencies == std::vector<int> { 0, 64, 0, 64, 0, 64 });
}

TEST_CASE ("Songs loaded while the audio thread runs are all taken without housekeeping", "[engine][transport]")
{
    TempPackage package;
    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = 256 } };
    const auto song = twoTrackSong (package, 1000);
    engine.loadSong (song, package.root);
    engine.play();

    std::atomic<bool> running { true };
    std::thread audioThread { [&] {
        DeviceDriver device { 2, 256 };
        while (running.load())
            device.processOneBlock (engine);
    } };

    // Two loads back to back, so the audio thread may take the first while the second is being
    // published. The second must still be taken by a following callback.
    int stalledAt = -1;
    for (int round = 0; round < 40 && stalledAt < 0; ++round)
    {
        const auto targetIsDelayed = round % 2 == 0;
        engine.loadSong (song, package.root, targetIsDelayed ? overdub::engine::InsertFactory {} : delayOnTrackB());
        engine.loadSong (song, package.root, targetIsDelayed ? delayOnTrackB() : overdub::engine::InsertFactory {});

        const auto targetLatency = targetIsDelayed ? 64 : 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (2);
        while (engine.latencySamples() != targetLatency && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for (std::chrono::microseconds (200));
        if (engine.latencySamples() != targetLatency)
            stalledAt = round;
    }

    running.store (false);
    audioThread.join();
    CHECK (stalledAt == -1);
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
