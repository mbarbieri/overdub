#include "engine/Engine.h"
#include "song/ClipEdits.h"

#include "EngineFixture.h"

#include <catch2/catch_test_macros.hpp>

// The Edit timeline's Clip operations (issue #28) heard through the engine (ADR 0005): a split
// nulls against the unsplit Clip, a moved impulse lands sample-exact, and a trim exposes the
// layer below, nulling against that layer alone.
using overdub::engine::Engine;
using overdub::engine::EngineConfig;
using namespace overdub::engine::testing;
namespace song = overdub::song;

namespace
{
constexpr int blockSize = 256;
constexpr int numBlocks = 40; // 10240 samples

// Channel 0 of the Song rendered from sample 0 (no Insert Chains: no latency).
std::vector<float> render (const song::Song& songToRender, const TempPackage& package)
{
    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = blockSize } };
    engine.loadSong (songToRender, package.root);
    engine.play();
    DeviceDriver device { 2, blockSize };
    return device.render (engine, numBlocks).channels[0];
}

std::vector<float> slice (const std::vector<float>& samples, std::int64_t from, std::int64_t to)
{
    return { samples.begin() + from, samples.begin() + to };
}

song::Song unwrap (overdub::core::Expected<song::Song> result)
{
    REQUIRE (result.has_value());
    return std::move (*result);
}
} // namespace

TEST_CASE ("rendering a Clip before and after a split nulls", "[engine][clip_edits]")
{
    TempPackage package;
    auto before = song::makeEmptySong (testSampleRate);
    const auto take = addTake (before, package, "take-a", sine (9000, 997.0));
    before.tracks.push_back (trackWith ("track-a", { clipOf ("clip-a", take, 1000, 500, 6000) }));

    const auto after = unwrap (song::splitClip (before, "clip-a", 3333, "clip-b"));
    REQUIRE (after.tracks[0].clips.size() == 2);

    const auto unsplit = render (before, package);
    const auto split = render (after, package);
    REQUIRE (! nonZeroSamples (unsplit).empty());
    CHECK (peakResidualDbfs (unsplit, split) <= -120.0);
}

TEST_CASE ("a moved Clip's impulse lands at the new position, sample-exact", "[engine][clip_edits]")
{
    TempPackage package;
    auto before = song::makeEmptySong (testSampleRate);
    // Source impulse at 300, read from 100: it sits 200 samples into the Clip.
    const auto take = addTake (before, package, "take-a", impulse (2000, 300));
    before.tracks.push_back (trackWith ("track-a", { clipOf ("clip-a", take, 1000, 100, 1500) }));
    CHECK (nonZeroSamples (render (before, package)) == std::vector<std::int64_t> { 1200 });

    const auto moved = unwrap (song::moveClip (before, "clip-a", 4321));
    CHECK (nonZeroSamples (render (moved, package)) == std::vector<std::int64_t> { 4521 });
}

TEST_CASE ("trimming the top Clip exposes the layer below, which nulls against that layer alone", "[engine][clip_edits]")
{
    TempPackage package;
    auto layered = song::makeEmptySong (testSampleRate);
    const auto lowTake = addTake (layered, package, "take-low", sine (8000, 440.0));
    const auto highTake = addTake (layered, package, "take-high", sine (8000, 1000.0, 0.3f));
    const auto low = clipOf ("clip-low", lowTake, 0, 0, 7000);
    const auto high = clipOf ("clip-high", highTake, 1000, 1000, 5000); // [1000, 6000), on top
    layered.tracks.push_back (trackWith ("track-a", { low, high }));

    auto lowAlone = layered;
    lowAlone.tracks[0].clips = { low };
    const auto expectedLow = render (lowAlone, package);

    SECTION ("trimming the start")
    {
        const auto trimmed = unwrap (song::trimClipStart (layered, "clip-high", 3000));
        const auto rendered = render (trimmed, package);
        CHECK (peakResidualDbfs (slice (rendered, 1000, 3000), slice (expectedLow, 1000, 3000)) <= -120.0);
        // The rest of the top Clip still hides the layer below.
        CHECK (peakResidualDbfs (slice (rendered, 3000, 6000), slice (expectedLow, 3000, 6000)) > -20.0);
    }

    SECTION ("trimming the end")
    {
        const auto trimmed = unwrap (song::trimClipEnd (layered, "clip-high", 4500));
        const auto rendered = render (trimmed, package);
        CHECK (peakResidualDbfs (slice (rendered, 4500, 6000), slice (expectedLow, 4500, 6000)) <= -120.0);
        CHECK (peakResidualDbfs (slice (rendered, 1000, 4500), slice (expectedLow, 1000, 4500)) > -20.0);
    }
}
