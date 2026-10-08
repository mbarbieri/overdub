#include "plugin/InsertHost.h"

#include "engine/Engine.h"
#include "engine/EngineFixture.h"
#include "engine/TestProcessors.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <memory>
#include <string>
#include <vector>

using overdub::engine::Engine;
using overdub::engine::EngineConfig;
using overdub::plugin::InsertHost;
using overdub::plugin::InsertStatus;
using overdub::song::InsertSlot;
using overdub::song::Song;
using namespace overdub::engine::testing;

// A bypassed Insert (issue #34): the engine plays its chain as if the slot were not there, latency
// included, while its plugin stays loaded with its state. Null tests (ADR 0005): ≤ -120 dBFS.
namespace
{
constexpr int blockSize = 256;
constexpr int renderBlocks = 40;
constexpr int pluginDelay = 64;

// An Instantiator that creates the test processor named by the slot's subtype at once: "dely" a
// 64-sample delay reporting that latency, "gain" a quarter gain.
overdub::plugin::Instantiator testInstantiator()
{
    return [] (const InsertSlot& slot, double, int, overdub::plugin::ProcessorReady ready) {
        if (slot.component.subtype == "dely")
            ready (std::unique_ptr<juce::AudioProcessor> (std::make_unique<DelayProcessor> (pluginDelay)));
        else
            ready (std::unique_ptr<juce::AudioProcessor> (std::make_unique<GainProcessor> (0.25f)));
    };
}

InsertSlot slotOf (const std::string& id, const std::string& subtype, bool bypassed)
{
    return InsertSlot { .id = id, .component = { "aufx", subtype, "ovdb" }, .name = id, .bypassed = bypassed };
}

// One Track with an impulse at 1200 and a sine after it, so a gain or a shift shows.
Song testSong (const TempPackage& package)
{
    auto song = overdub::song::makeEmptySong (testSampleRate);
    auto samples = impulse (8000, 1200);
    const auto tone = sine (8000, 440.0, 0.3f);
    for (std::size_t i = 3000; i < samples.size(); ++i)
        samples[i] = tone[i];
    const auto take = addTake (song, package, "take-a", samples);
    song.tracks.push_back (trackWith ("track-a", { clipOf ("clip-a", take, 0, 0, 8000) }));
    return song;
}

struct Rendered
{
    RenderedOutput output;
    int latency = 0;
};

Rendered render (const Song& song, const TempPackage& package, const overdub::engine::InsertFactory& inserts)
{
    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = blockSize } };
    engine.loadSong (song, package.root, inserts);
    engine.play();
    DeviceDriver device { 2, blockSize };
    auto output = device.render (engine, renderBlocks);
    return { std::move (output), engine.latencySamples() };
}
} // namespace

TEST_CASE ("a bypassed Insert nulls against no Insert, on a Track and on the Master", "[plugin][insert_host][bypass]")
{
    TempPackage package;
    const auto plain = testSong (package);
    const auto reference = render (plain, package, {});

    auto bypassed = plain;
    bypassed.tracks[0].insertChain = { slotOf ("delay", "dely", true), slotOf ("gain", "gain", true) };
    bypassed.master.insertChain = { slotOf ("master-gain", "gain", true) };
    InsertHost host { testInstantiator(), {}, testSampleRate, blockSize };
    host.sync (bypassed);
    REQUIRE (host.status ("delay") == InsertStatus::ready);
    REQUIRE (host.status ("master-gain") == InsertStatus::ready);

    const auto heard = render (bypassed, package, host.insertFactory());

    CHECK (heard.latency == 0);
    for (std::size_t channel = 0; channel < 2; ++channel)
        CHECK (peakResidualDbfs (heard.output.channels[channel], reference.output.channels[channel]) <= -120.0);
}

TEST_CASE ("an Insert back from bypass is heard again with its latency, from the same plugin", "[plugin][insert_host][bypass]")
{
    TempPackage package;
    auto song = testSong (package);
    song.tracks[0].insertChain = { slotOf ("delay", "dely", true), slotOf ("gain", "gain", true) };
    InsertHost host { testInstantiator(), {}, testSampleRate, blockSize };
    host.sync (song);
    auto* gain = host.processor ("gain");

    song.tracks[0].insertChain[0].bypassed = false;
    song.tracks[0].insertChain[1].bypassed = false;
    host.sync (song);
    const auto heard = render (song, package, host.insertFactory());

    CHECK (host.processor ("gain") == gain);
    CHECK (heard.latency == pluginDelay);
    // PDC keeps the impulse at its Song position; the gain is heard.
    CHECK (nonZeroSamples (heard.output.channels[0], 1e-6f).front() == 1200);
    CHECK_THAT (heard.output.channels[0][1200], Catch::Matchers::WithinAbs (0.25 * centrePanGain, 1e-7));
}
