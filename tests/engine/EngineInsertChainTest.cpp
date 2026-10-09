#include "engine/Engine.h"
#include "engine/SharedInsertNode.h"

#include "EngineFixture.h"
#include "TestProcessors.h"
#include "support/RealtimeGuardCatch.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <atomic>
#include <memory>

using overdub::engine::Engine;
using overdub::engine::EngineConfig;
using overdub::engine::InsertFactory;
using overdub::engine::SharedInsertNode;
using namespace overdub::engine::testing;

// Insert Chains in the graph (issue #33): the Master's chain, tails at stop, and plugins shared
// across plans through SharedInsertNode. AU tests are in tests/plugin/.
namespace
{
constexpr int blockSize = 256;

EngineConfig stereoConfig()
{
    return EngineConfig { .numOutputChannels = 2, .maxBlockSize = blockSize };
}

// One Track with an impulse at `impulseAt`.
overdub::song::Song impulseSong (const TempPackage& package, std::int64_t impulseAt, std::int64_t length = 8000)
{
    auto song = overdub::song::makeEmptySong (testSampleRate);
    const auto take = addTake (song, package, "take-a", impulse (length, impulseAt));
    song.tracks.push_back (trackWith ("track-a", { clipOf ("clip-a", take, 0, 0, length) }));
    return song;
}

// An InsertFactory that puts the same shared processor on the Track with this id, in every plan.
InsertFactory sharedOnTrack (std::string trackId, std::shared_ptr<juce::AudioProcessor> processor)
{
    return [trackId, processor] (const overdub::song::Id& chainTrackId, const std::vector<overdub::song::InsertSlot>&) {
        std::vector<std::unique_ptr<juce::AudioProcessor>> inserts;
        if (chainTrackId == trackId)
            inserts.push_back (std::make_unique<SharedInsertNode> (processor));
        return inserts;
    };
}

std::vector<float> renderSamples (DeviceDriver& device, Engine& engine, int numBlocks)
{
    return device.render (engine, numBlocks).channels[0];
}
} // namespace

TEST_CASE ("the Master's Insert Chain shapes the whole mix and its latency is compensated", "[engine][pdc][master]")
{
    TempPackage package;
    auto song = impulseSong (package, 1200);
    const auto takeB = addTake (song, package, "take-b", impulse (8000, 2000));
    song.tracks.push_back (trackWith ("track-b", { clipOf ("clip-b", takeB, 0, 0, 8000) }));

    static constexpr int masterDelay = 300;
    Engine engine { stereoConfig() };
    engine.loadSong (song, package.root, [] (const overdub::song::Id& trackId, const std::vector<overdub::song::InsertSlot>&) {
        std::vector<std::unique_ptr<juce::AudioProcessor>> inserts;
        if (trackId.empty())
        {
            inserts.push_back (std::make_unique<DelayProcessor> (masterDelay));
            inserts.push_back (std::make_unique<GainProcessor> (0.5f));
        }
        return inserts;
    });
    engine.play();

    DeviceDriver device { 2, blockSize };
    const auto rendered = device.render (engine, 16);

    CHECK (engine.latencySamples() == masterDelay);
    // Both Tracks pass through the Master's chain: halved, and still at their Song positions.
    for (const auto& channel : rendered.channels)
    {
        CHECK (nonZeroSamples (channel) == std::vector<std::int64_t> { 1200, 2000 });
        CHECK_THAT (channel[1200], Catch::Matchers::WithinULP (0.5f * centrePanGain, 0));
        CHECK_THAT (channel[2000], Catch::Matchers::WithinULP (0.5f * centrePanGain, 0));
    }
}

TEST_CASE ("a Track's and the Master's plugin latencies add up and are compensated sample-exact", "[engine][pdc][master]")
{
    TempPackage package;
    auto song = impulseSong (package, 1500);
    const auto takeB = addTake (song, package, "take-b", impulse (8000, 1700));
    song.tracks.push_back (trackWith ("track-b", { clipOf ("clip-b", takeB, 0, 0, 8000) }));

    Engine engine { stereoConfig() };
    engine.loadSong (song, package.root, [] (const overdub::song::Id& trackId, const std::vector<overdub::song::InsertSlot>&) {
        std::vector<std::unique_ptr<juce::AudioProcessor>> inserts;
        if (trackId.empty())
            inserts.push_back (std::make_unique<DelayProcessor> (64));
        else if (trackId == "track-b")
            inserts.push_back (std::make_unique<DelayProcessor> (100));
        return inserts;
    });
    engine.play();

    DeviceDriver device { 2, blockSize };
    const auto rendered = renderSamples (device, engine, 16);

    CHECK (engine.latencySamples() == 164);
    CHECK (nonZeroSamples (rendered) == std::vector<std::int64_t> { 1500, 1700 });
}

TEST_CASE ("an Insert's tail rings out after the transport stops", "[engine][tail]")
{
    TempPackage package;
    static constexpr int echoDelay = 2000;
    Engine engine { stereoConfig() };
    engine.loadSong (impulseSong (package, 1000), package.root, insertOnTrack ("track-a", [] { return std::make_unique<EchoProcessor> (echoDelay, testSampleRate); }));
    engine.play();

    DeviceDriver device { 2, blockSize };
    auto rendered = renderSamples (device, engine, 6); // to Song sample 1536: past the impulse, before its echo
    engine.stop();
    const auto afterStop = renderSamples (device, engine, 10);
    rendered.insert (rendered.end(), afterStop.begin(), afterStop.end());

    CHECK (engine.tailSamples() == echoDelay);
    CHECK_FALSE (engine.isPlaying());
    // The echo of the impulse at 1000 comes out 2000 samples later, while stopped.
    CHECK (nonZeroSamples (rendered) == std::vector<std::int64_t> { 1000, 1000 + echoDelay });
}

TEST_CASE ("a plan's tail is its longest Track chain tail plus the Master chain's", "[engine][tail]")
{
    TempPackage package;
    auto song = impulseSong (package, 100);
    song.tracks.push_back (trackWith ("track-b", {}));

    Engine engine { stereoConfig() };
    engine.loadSong (song, package.root, [] (const overdub::song::Id& trackId, const std::vector<overdub::song::InsertSlot>&) {
        std::vector<std::unique_ptr<juce::AudioProcessor>> inserts;
        if (trackId.empty())
            inserts.push_back (std::make_unique<EchoProcessor> (480, testSampleRate));
        else if (trackId == "track-a")
            inserts.push_back (std::make_unique<EchoProcessor> (1000, testSampleRate));
        else
        {
            inserts.push_back (std::make_unique<EchoProcessor> (700, testSampleRate));
            inserts.push_back (std::make_unique<EchoProcessor> (600, testSampleRate));
        }
        return inserts;
    });
    DeviceDriver device { 2, blockSize };
    device.processOneBlock (engine);

    CHECK (engine.tailSamples() == 1300 + 480);
}

TEST_CASE ("a shared Insert keeps its state across plan swaps and is not run by a plan's warm-up", "[engine][shared_insert]")
{
    TempPackage package;
    const auto song = impulseSong (package, 1000);
    static constexpr int echoDelay = 2000;
    // The plugin host prepares a shared processor; plans never do.
    auto echo = std::make_shared<EchoProcessor> (echoDelay, testSampleRate);
    echo->prepareToPlay (testSampleRate, blockSize);

    Engine engine { stereoConfig() };
    engine.loadSong (song, package.root, sharedOnTrack ("track-a", echo));
    engine.play();

    DeviceDriver device { 2, blockSize };
    auto rendered = renderSamples (device, engine, 6); // past the impulse, before its echo
    // A rebuilt plan (any Song edit) takes the same processor: the echo it owes is still heard,
    // exactly on time, so the new plan's warm-up did not run it.
    engine.loadSong (song, package.root, sharedOnTrack ("track-a", echo));
    const auto afterSwap = renderSamples (device, engine, 10);
    rendered.insert (rendered.end(), afterSwap.begin(), afterSwap.end());

    CHECK (nonZeroSamples (rendered) == std::vector<std::int64_t> { 1000, 1000 + echoDelay });
}

TEST_CASE ("a shared Insert reports its processor's latency, which PDC compensates", "[engine][shared_insert][pdc]")
{
    TempPackage package;
    auto song = impulseSong (package, 1200);
    const auto takeB = addTake (song, package, "take-b", impulse (8000, 2000));
    song.tracks.push_back (trackWith ("track-b", { clipOf ("clip-b", takeB, 0, 0, 8000) }));
    auto delay = std::make_shared<DelayProcessor> (128);
    delay->prepareToPlay (testSampleRate, blockSize);

    Engine engine { stereoConfig() };
    engine.loadSong (song, package.root, sharedOnTrack ("track-b", delay));
    engine.play();
    DeviceDriver device { 2, blockSize };
    const auto rendered = renderSamples (device, engine, 16);

    CHECK (engine.latencySamples() == 128);
    CHECK (nonZeroSamples (rendered) == std::vector<std::int64_t> { 1200, 2000 });
}

TEST_CASE ("process() neither allocates nor locks with shared and Master Inserts in the graph", "[engine][shared_insert][rt]")
{
    TempPackage package;
    auto echo = std::make_shared<EchoProcessor> (500, testSampleRate);
    echo->prepareToPlay (testSampleRate, blockSize);
    const auto shared = sharedOnTrack ("track-a", echo);

    Engine engine { stereoConfig() };
    engine.loadSong (impulseSong (package, 1000), package.root, [shared] (const overdub::song::Id& trackId, const std::vector<overdub::song::InsertSlot>& chain) {
        auto inserts = shared (trackId, chain);
        if (trackId.empty())
            inserts.push_back (std::make_unique<DelayProcessor> (64));
        return inserts;
    });
    DeviceDriver device { 2, blockSize };
    REQUIRE_REALTIME_SAFE (device.processOneBlock (engine));
    engine.play();
    REQUIRE_REALTIME_SAFE (for (int block = 0; block < 8; ++block) device.processOneBlock (engine));
    engine.stop();
    REQUIRE_REALTIME_SAFE (for (int block = 0; block < 4; ++block) device.processOneBlock (engine));
}

TEST_CASE ("a shared Insert whose plugin crashed passes its audio through at the latency the plan gave it", "[engine][shared_insert][crash]")
{
    TempPackage package;
    auto song = impulseSong (package, 1200);
    const auto takeB = addTake (song, package, "take-b", impulse (8000, 2000));
    song.tracks.push_back (trackWith ("track-b", { clipOf ("clip-b", takeB, 0, 0, 8000) }));
    // A dead plugin's output is garbage (here silence): once crashed it must not be run at all.
    auto dead = std::make_shared<SilenceProcessor>();
    dead->setLatencySamples (128);
    auto crashed = std::make_shared<std::atomic<bool>> (false);

    Engine engine { stereoConfig() };
    engine.loadSong (song, package.root, [dead, crashed] (const overdub::song::Id& trackId, const std::vector<overdub::song::InsertSlot>&) {
        std::vector<std::unique_ptr<juce::AudioProcessor>> inserts;
        if (trackId == "track-b")
            inserts.push_back (std::make_unique<SharedInsertNode> (dead, crashed));
        return inserts;
    });
    engine.play();
    DeviceDriver device { 2, blockSize };
    auto rendered = renderSamples (device, engine, 4); // to Song sample 1024
    crashed->store (true);
    const auto afterCrash = renderSamples (device, engine, 12);
    rendered.insert (rendered.end(), afterCrash.begin(), afterCrash.end());

    CHECK (engine.latencySamples() == 128);
    // Track B's impulse is heard dry, still in time with Track A: the plan's PDC still holds.
    CHECK (nonZeroSamples (rendered) == std::vector<std::int64_t> { 1200, 2000 });
    REQUIRE_REALTIME_SAFE (for (int block = 0; block < 4; ++block) device.processOneBlock (engine));
}
