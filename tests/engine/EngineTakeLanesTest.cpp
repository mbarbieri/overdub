#include "engine/Engine.h"
#include "song/TakeEdits.h"

#include "EngineFixture.h"

#include <catch2/catch_test_macros.hpp>

// Swap and bring back heard through the engine (issue #30, ADR 0005): after a Swap the render
// over the Clip's span nulls against the chosen Take at that span, with the Clip's gain and its
// clamped fades, and nothing sounds where the chosen Take has no audio.
using overdub::engine::Engine;
using overdub::engine::EngineConfig;
using namespace overdub::engine::testing;
namespace song = overdub::song;

namespace
{
constexpr int blockSize = 256;
constexpr int numBlocks = 48; // 12288 samples

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

// Two Passes on Track "g": Pass 1 from 0 (take "a", a 440 Hz sine, origin 0, 10000 samples) and
// Pass 2 from 2000 (take "b", a 1 kHz sine, origin 1700, so its recorded span is [2000, 6000)).
// One Clip "c" plays take "a" over [1000, 9000), at -3 dB with a 500-sample fade-in and a
// 6000-sample fade-out.
song::Song twoPassSong (const TempPackage& package)
{
    auto built = song::makeEmptySong (testSampleRate);
    built.passes = { song::Pass { "p1", 0, "" }, song::Pass { "p2", 2000, "" } };
    const auto a = addTake (built, package, "a", sine (10000, 440.0));
    addTake (built, package, "b", sine (4300, 1000.0, 0.3f));
    built.takes[0].trackId = "g";
    built.takes[0].passId = "p1";
    built.takes[1].trackId = "g";
    built.takes[1].passId = "p2";
    built.takes[1].origin = 1700;

    auto clip = clipOf ("c", a, 1000, 1000, 8000);
    clip.gainDb = -3.0;
    clip.fadeInLength = 500;
    clip.fadeOutLength = 6000;
    built.tracks.push_back (trackWith ("g", { clip }));
    return built;
}
} // namespace

TEST_CASE ("after a Swap the Clip's span nulls against the chosen Take there, with its gain and clamped fades", "[engine][take_lanes]")
{
    TempPackage package;
    const auto before = twoPassSong (package);
    const auto swapped = song::swapClipTake (before, "c", "b");
    REQUIRE (swapped.has_value());

    const auto& clip = swapped->tracks[0].clips[0];
    CHECK (clip.timelineStart == 2000);
    CHECK (clip.length == 4000);
    CHECK (clip.sourceOffset == 300);
    CHECK (clip.fadeInLength == 500);
    CHECK (clip.fadeOutLength == 3500);

    // The chosen Take alone at that span: Take "b" read where it was recorded, same gain and fades.
    auto reference = *swapped;
    reference.tracks[0].clips = { clipOf ("ref", reference.takes[1], 2000, 2000 - 1700, 4000) };
    reference.tracks[0].clips[0].gainDb = -3.0;
    reference.tracks[0].clips[0].fadeInLength = 500;
    reference.tracks[0].clips[0].fadeOutLength = 3500;

    const auto rendered = render (*swapped, package);
    const auto expected = render (reference, package);
    REQUIRE (nonZeroSamples (slice (expected, 2000, 6000)).size() > 3000);
    CHECK (peakResidualDbfs (slice (rendered, 2000, 6000), slice (expected, 2000, 6000)) <= -120.0);
    // Trimmed to the Take's audio: silence where Take "a" played before.
    CHECK (nonZeroSamples (slice (rendered, 0, 2000)).empty());
    CHECK (nonZeroSamples (slice (rendered, 6000, 12000)).empty());
}

TEST_CASE ("a brought-back Pass sounds as recorded on top of what was there", "[engine][take_lanes]")
{
    TempPackage package;
    const auto before = twoPassSong (package);
    const auto brought = song::bringBackPass (before, "p2");
    REQUIRE (brought.has_value());
    REQUIRE (brought->tracks[0].clips.size() == 2);

    auto takeBAlone = *brought;
    takeBAlone.tracks[0].clips = { brought->tracks[0].clips[1] };
    const auto rendered = render (*brought, package);
    const auto expected = render (takeBAlone, package);
    CHECK (peakResidualDbfs (slice (rendered, 2000, 6000), slice (expected, 2000, 6000)) <= -120.0);
    // Outside it, the older Clip still plays.
    CHECK (! nonZeroSamples (slice (rendered, 1000, 2000)).empty());
}
