#include "engine/Engine.h"
#include "engine/EngineEvents.h"

#include "EngineFixture.h"
#include "TestProcessors.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <optional>
#include <string>
#include <thread>

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

// Holds a processor's callback lock on another thread from construction until destruction, as
// a host thread calling suspendProcessing or a plugin's own state call would.
class CallbackLockHeldElsewhere
{
public:
    explicit CallbackLockHeldElsewhere (juce::AudioProcessor& processor)
        : holder ([this, &processor]
                  {
                      const juce::ScopedLock lock { processor.getCallbackLock() };
                      held.store (true);
                      while (! released.load())
                          std::this_thread::sleep_for (std::chrono::microseconds (100));
                  })
    {
        while (! held.load())
            std::this_thread::sleep_for (std::chrono::microseconds (100));
    }

    ~CallbackLockHeldElsewhere()
    {
        released.store (true);
        holder.join();
    }

private:
    std::atomic<bool> held { false };
    std::atomic<bool> released { false };
    std::thread holder;
};

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

TEST_CASE ("a node whose callback lock is busy is silenced and reported, not waited for", "[engine][rt][events]")
{
    ScopedLogCapture capture;
    TempPackage package;
    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = 256 } };
    juce::AudioProcessor* gainNode = nullptr;
    engine.loadSong (oneTrackSong (package), package.root, insertOnTrack ("track-a", [&gainNode]
                                                                          {
                                                                              auto gain = std::make_unique<GainProcessor> (1.0f);
                                                                              gainNode = gain.get();
                                                                              return gain;
                                                                          }));
    REQUIRE (gainNode != nullptr);
    engine.play();
    DeviceDriver device { 2, 256 };
    device.hostTimeNs = 3'000'000'000;
    constexpr std::uint64_t second = 1'000'000'000;
    engine.housekeeping (100 * second);

    std::optional<CallbackLockHeldElsewhere> held;
    held.emplace (*gainNode);
    const auto firstBusyHostNs = device.hostTimeNs;
    const auto busy = device.render (engine, 2); // Song samples 0..511: the impulse at 10 is lost
    held.reset();
    device.render (engine, 1); // 512..767: the lock is free again
    held.emplace (*gainNode);
    const auto secondBusyHostNs = device.hostTimeNs;
    device.render (engine, 1); // 768..1023: busy again
    held.reset();

    engine.housekeeping (101 * second);
    drain (engine);

    CHECK (nonZeroSamples (busy.channels[0]).empty());
    const auto silenced = eventsNamed (capture, "engine.nodes_silenced");
    REQUIRE (silenced.size() == 2);
    CHECK (silenced[0]["lvl"] == "warn");
    CHECK (silenced[0]["sample"] == 0);
    CHECK (silenced[0]["host_ns"] == firstBusyHostNs);
    CHECK (silenced[0]["node_blocks"] == 1.0);
    CHECK (silenced[1]["sample"] == 768);
    CHECK (silenced[1]["host_ns"] == secondBusyHostNs);

    const auto load = eventsNamed (capture, "engine.load");
    REQUIRE (load.size() == 1);
    CHECK (load[0]["silenced_node_blocks"] == 3);
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
