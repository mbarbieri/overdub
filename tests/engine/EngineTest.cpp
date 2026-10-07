#include "engine/Engine.h"

#include "EngineFixture.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using overdub::engine::Engine;
using overdub::engine::EngineConfig;
using namespace overdub::engine::testing;

namespace
{
EngineConfig stereoConfig (int maxBlockSize = 256)
{
    return EngineConfig { .numOutputChannels = 2, .maxBlockSize = maxBlockSize };
}
} // namespace

TEST_CASE ("an impulse in a Clip comes out at its timeline position", "[engine][playback]")
{
    TempPackage package;
    auto song = overdub::song::makeEmptySong (testSampleRate);
    // Source impulse at 300; the Clip starts reading the Take at 100 and sits at timeline 1000,
    // so the impulse belongs at song sample 1000 + (300 - 100) = 1200.
    const auto take = addTake (song, package, "take-a", impulse (2000, 300));
    song.tracks.push_back (trackWith ("track-a", { clipOf ("clip-a", take, 1000, 100, 1500) }));

    Engine engine { stereoConfig() };
    engine.loadSong (song, package.root);
    engine.play();

    DeviceDriver device { 2, 256 };
    const auto rendered = device.render (engine, 12);

    CHECK (nonZeroSamples (rendered.channels[0]) == std::vector<std::int64_t> { 1200 });
    CHECK (nonZeroSamples (rendered.channels[1]) == std::vector<std::int64_t> { 1200 });
    CHECK_THAT (rendered.channels[0][1200], Catch::Matchers::WithinULP (1.0f, 0));
}
