#include "engine/Engine.h"
#include "engine/EngineEvents.h"

#include "EngineFixture.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using overdub::core::ScopedLogCapture;
using overdub::engine::Engine;
using overdub::engine::EngineConfig;
using namespace overdub::engine::testing;

namespace
{
std::vector<nlohmann::json> eventsNamed (const ScopedLogCapture& capture, const std::string& name)
{
    std::vector<nlohmann::json> found;
    for (const auto& event : capture.events())
        if (event.at ("event") == name)
            found.push_back (event);
    return found;
}

void drain (Engine& engine)
{
    overdub::core::drainRtEvents (engine.rtEvents(), overdub::engine::engineRtCatalog);
}

overdub::song::Song oneTrackSong (const TempPackage& package)
{
    auto song = overdub::song::makeEmptySong (testSampleRate);
    const auto take = addTake (song, package, "take-a", impulse (2000, 10));
    song.tracks.push_back (trackWith ("track-a", { clipOf ("clip-a", take, 0, 0, 2000) }));
    return song;
}
} // namespace

TEST_CASE ("transport RT events carry the block's Song position and host time", "[engine][rt][events]")
{
    ScopedLogCapture capture;
    TempPackage package;
    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = 256 } };
    engine.loadSong (oneTrackSong (package), package.root);

    DeviceDriver device { 2, 256 };
    device.hostTimeNs = 7'000'000'000;
    device.processOneBlock (engine); // takes the plan at Song sample 0, host 7 s

    engine.play();
    const auto playHostNs = device.hostTimeNs;
    device.render (engine, 2); // started at 0; position now 512

    engine.seek (4096);
    const auto seekHostNs = device.hostTimeNs;
    device.processOneBlock (engine); // seeked to 4096 from 512; position now 4352

    engine.stop();
    const auto stopHostNs = device.hostTimeNs;
    device.processOneBlock (engine);

    drain (engine);

    const auto swapped = eventsNamed (capture, "engine.plan_swapped");
    REQUIRE (swapped.size() == 1);
    CHECK (swapped[0]["sample"] == 0);
    CHECK (swapped[0]["host_ns"] == 7'000'000'000ULL);
    CHECK (swapped[0]["latency_samples"] == 0.0);
    CHECK (swapped[0]["tracks"] == 1.0);

    const auto started = eventsNamed (capture, "engine.transport_started");
    REQUIRE (started.size() == 1);
    CHECK (started[0]["sample"] == 0);
    CHECK (started[0]["host_ns"] == playHostNs);

    const auto seeked = eventsNamed (capture, "engine.seeked");
    REQUIRE (seeked.size() == 1);
    CHECK (seeked[0]["sample"] == 4096);
    CHECK (seeked[0]["host_ns"] == seekHostNs);
    CHECK (seeked[0]["from_sample"] == 512.0);

    const auto stopped = eventsNamed (capture, "engine.transport_stopped");
    REQUIRE (stopped.size() == 1);
    CHECK (stopped[0]["sample"] == 4352);
    CHECK (stopped[0]["host_ns"] == stopHostNs);
}

TEST_CASE ("a block larger than the plan was prepared for comes out silent and is reported", "[engine][rt][events]")
{
    ScopedLogCapture capture;
    TempPackage package;
    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = 128 } };
    engine.loadSong (oneTrackSong (package), package.root);
    engine.play();

    DeviceDriver device { 2, 256 };
    device.hostTimeNs = 9'000'000'000;
    const auto rendered = device.render (engine, 1);
    drain (engine);

    CHECK (nonZeroSamples (rendered.channels[0]).empty());
    const auto rejected = eventsNamed (capture, "engine.block_rejected");
    REQUIRE (rejected.size() == 1);
    CHECK (rejected[0]["lvl"] == "error");
    CHECK (rejected[0]["sample"] == 0);
    CHECK (rejected[0]["host_ns"] == 9'000'000'000ULL);
    CHECK (rejected[0]["num_samples"] == 256.0);
    CHECK (rejected[0]["output_channels"] == 2.0);
    CHECK (rejected[0]["max_block_size"] == 128.0);
    CHECK (rejected[0]["plan_output_channels"] == 2.0);
    // The clock keeps running: the transport still advanced by the rejected block.
    CHECK (engine.position() == 256);
}

TEST_CASE ("rejected blocks are reported once when the rejection starts or changes shape", "[engine][rt][events]")
{
    ScopedLogCapture capture;
    TempPackage package;
    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = 128 } };
    engine.loadSong (oneTrackSong (package), package.root);
    engine.play();

    DeviceDriver tooLarge { 2, 256 };
    DeviceDriver evenLarger { 2, 512 };
    DeviceDriver fitting { 2, 128 };
    tooLarge.render (engine, 3);   // rejection starts at Song sample 0: reported
    evenLarger.render (engine, 3); // new shape at 768: reported
    fitting.render (engine, 1);    // plays at 2304
    tooLarge.render (engine, 2);   // rejection starts again at 2432: reported
    drain (engine);

    const auto rejected = eventsNamed (capture, "engine.block_rejected");
    REQUIRE (rejected.size() == 3);
    CHECK (rejected[0]["sample"] == 0);
    CHECK (rejected[0]["num_samples"] == 256.0);
    CHECK (rejected[1]["sample"] == 768);
    CHECK (rejected[1]["num_samples"] == 512.0);
    CHECK (rejected[2]["sample"] == 2432);
    CHECK (rejected[2]["num_samples"] == 256.0);
}

TEST_CASE ("engine RT events beyond the ring's capacity are counted and reported once", "[engine][rt][events]")
{
    ScopedLogCapture capture;
    TempPackage package;
    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = 256 }, 2 };
    engine.loadSong (oneTrackSong (package), package.root);

    DeviceDriver device { 2, 256 };
    device.processOneBlock (engine); // plan_swapped
    engine.play();
    device.processOneBlock (engine); // transport_started
    engine.seek (0);
    device.processOneBlock (engine); // seeked: dropped
    engine.stop();
    device.processOneBlock (engine); // transport_stopped: dropped
    drain (engine);

    CHECK (eventsNamed (capture, "engine.plan_swapped").size() == 1);
    CHECK (eventsNamed (capture, "engine.transport_started").size() == 1);
    CHECK (eventsNamed (capture, "engine.seeked").empty());
    const auto dropped = eventsNamed (capture, "engine.rt_events_dropped");
    REQUIRE (dropped.size() == 1);
    CHECK (dropped[0]["dropped"] == 2);
}

TEST_CASE ("every callback publishes its anchor: Song sample at buffer start and output host time", "[engine][anchor]")
{
    TempPackage package;
    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = 256 } };
    CHECK_FALSE (engine.latestAnchor().has_value());

    engine.loadSong (oneTrackSong (package), package.root);
    DeviceDriver device { 2, 256 };
    device.hostTimeNs = 5'000'000'000;

    device.processOneBlock (engine);
    CHECK (engine.latestAnchor() == overdub::engine::Anchor { 0, 5'000'000'000 });

    engine.play();
    device.processOneBlock (engine);
    const auto secondHostNs = 5'000'000'000 + device.blockDurationNs();
    CHECK (engine.latestAnchor() == overdub::engine::Anchor { 0, secondHostNs });

    device.processOneBlock (engine);
    CHECK (engine.latestAnchor() == overdub::engine::Anchor { 256, secondHostNs + device.blockDurationNs() });

    engine.seek (10'000);
    device.processOneBlock (engine);
    CHECK (engine.latestAnchor() == overdub::engine::Anchor { 10'000, secondHostNs + 2 * device.blockDurationNs() });
}

TEST_CASE ("engine.load summarises callback load once per second, only while playing", "[engine][load]")
{
    ScopedLogCapture capture;
    TempPackage package;
    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = 256 } };
    engine.loadSong (oneTrackSong (package), package.root);
    DeviceDriver device { 2, 256 };

    constexpr std::uint64_t second = 1'000'000'000;
    const std::uint64_t start = 100 * second;
    engine.housekeeping (start);

    // Stopped: nothing is logged even when a second has passed.
    device.render (engine, 4);
    engine.housekeeping (start + second);
    CHECK (eventsNamed (capture, "engine.load").empty());

    engine.play();
    device.render (engine, 10);
    engine.housekeeping (start + second + second / 2); // under a second since the last summary
    CHECK (eventsNamed (capture, "engine.load").empty());

    engine.housekeeping (start + 2 * second);
    const auto load = eventsNamed (capture, "engine.load");
    REQUIRE (load.size() == 1);
    CHECK (load[0]["lvl"] == "debug");
    CHECK (load[0]["callbacks"] == 10);
    CHECK (load[0]["load"].get<double>() > 0.0);
    CHECK (load[0]["peak_callback_us"].get<double>() > 0.0);
}
