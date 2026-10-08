#include "export/MixRender.h"

#include "engine/Engine.h"
#include "engine/EngineFixture.h"
#include "engine/TestProcessors.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

// The offline render of a Song's mix (issue #37, ADR 0005 null tests): it must sound exactly like
// the real-time Engine::process() render of the same Song, through the same graph, so the Mixer,
// Clip gain and fades, PDC and Insert Chains are covered as they land.
using overdub::engine::Engine;
using overdub::engine::EngineConfig;
using overdub::engine::InsertFactory;
using overdub::exporting::MixRender;
using namespace overdub::engine::testing;
namespace song = overdub::song;

namespace
{
using Channels = std::vector<std::vector<float>>;

Channels renderOffline (MixRender& render, int blockSize)
{
    Channels out (MixRender::numChannels);
    std::vector<std::vector<float>> storage (MixRender::numChannels, std::vector<float> ((std::size_t) blockSize));
    std::vector<float*> pointers;
    for (auto& channel : storage)
        pointers.push_back (channel.data());
    while (true)
    {
        const auto count = render.renderNext (pointers.data(), blockSize);
        if (count == 0)
            break;
        for (std::size_t c = 0; c < storage.size(); ++c)
            out[c].insert (out[c].end(), storage[c].begin(), storage[c].begin() + count);
    }
    return out;
}

std::vector<float> firstSamples (const std::vector<float>& samples, std::int64_t count)
{
    REQUIRE ((std::int64_t) samples.size() >= count);
    return { samples.begin(), samples.begin() + count };
}

// Fresh test processors for every plan (live and offline), by chain:
//   Bass:   Delay 300 (reports 300 samples of latency) -> Gain 0.5 -> Echo 700 (tail 700)
//   Master: Echo 480 (tail 480) -> Delay 100
InsertFactory mixInserts()
{
    return [] (const song::Id& trackId, const std::vector<song::InsertSlot>&) {
        std::vector<std::unique_ptr<juce::AudioProcessor>> chain;
        if (trackId == "bass")
        {
            chain.push_back (std::make_unique<DelayProcessor> (300));
            chain.push_back (std::make_unique<GainProcessor> (0.5f));
            chain.push_back (std::make_unique<EchoProcessor> (700, testSampleRate));
        }
        if (trackId.empty())
        {
            chain.push_back (std::make_unique<EchoProcessor> (480, testSampleRate));
            chain.push_back (std::make_unique<DelayProcessor> (100));
        }
        return chain;
    };
}

// Three Tracks exercising the mix: Clip gain and fades, volume and pan, a muted Track.
song::Song mixSong (const TempPackage& package)
{
    auto mix = song::makeEmptySong (testSampleRate);
    const auto guitarTake = addTake (mix, package, "take-guitar", sine (30000, 997.0, 0.7f));
    const auto bassTake = addMultichannelTake (mix, package, "take-bass", { sine (20000, 110.0, 0.6f), sine (20000, 220.0, 0.4f) });
    const auto mutedTake = addTake (mix, package, "take-muted", sine (10000, 440.0, 0.9f));

    auto guitarClip = clipOf ("clip-guitar", guitarTake, 4800, 1000, 24000);
    guitarClip.gainDb = -3.0;
    guitarClip.fadeInLength = 1000;
    guitarClip.fadeOutLength = 2500;
    auto guitar = trackWith ("guitar", { guitarClip });
    guitar.volumeDb = -6.0;
    guitar.pan = -0.5;

    auto bass = trackWith ("bass", { clipOf ("clip-bass", bassTake, 9000, 0, 20000) });
    bass.pan = 0.3;

    auto muted = trackWith ("muted", { clipOf ("clip-muted", mutedTake, 6000, 0, 10000) });
    muted.mute = true;

    mix.tracks = { guitar, bass, muted };
    mix.master.volumeDb = -1.5;
    return mix;
}
} // namespace

TEST_CASE ("the Song span ends where its last Clip ends", "[export][span]")
{
    auto songToSpan = song::makeEmptySong (testSampleRate);
    CHECK (overdub::exporting::lastClipEnd (songToSpan) == 0);

    song::Take take;
    take.id = "take";
    songToSpan.tracks = { trackWith ("a", { clipOf ("a1", take, 100, 0, 900), clipOf ("a2", take, 50, 0, 100) }),
                          trackWith ("b", { clipOf ("b1", take, 2000, 0, 48) }),
                          trackWith ("c", {}) };
    CHECK (overdub::exporting::lastClipEnd (songToSpan) == 2048);
}

TEST_CASE ("the offline render nulls against the real-time process() render of the same Song", "[export][null]")
{
    TempPackage package;
    const auto mix = mixSong (package);

    MixRender offline { mix, package.root, 256, mixInserts() };
    REQUIRE (offline.latencySamples() == 400);
    REQUIRE (offline.songEnd() == 29000);
    REQUIRE (offline.tailSamples() == 700 + 480);
    REQUIRE (offline.totalSamples() == 29000 + 1180);
    const auto rendered = renderOffline (offline, 256);
    REQUIRE ((std::int64_t) rendered[0].size() == offline.totalSamples());
    CHECK (offline.renderedSamples() == offline.totalSamples());

    // Live: play from 0 with a device block that is not the plan's (128 of 256). After PDC output
    // sample n is Song sample n; the Song is silent before 4800, past the 400 samples a start mutes.
    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = 256 } };
    engine.loadSong (mix, package.root, mixInserts());
    engine.seek (0);
    engine.play();
    DeviceDriver driver { 2, 128 };
    const auto live = driver.render (engine, (int) (offline.totalSamples() / 128) + 2);
    REQUIRE (engine.latencySamples() == 400);

    const auto total = offline.totalSamples();
    for (std::size_t c = 0; c < 2; ++c)
    {
        CAPTURE (c);
        CHECK (nonZeroSamples (rendered[c], 1.0e-3f).size() > 10000); // the mix is really there
        CHECK (peakResidualDbfs (rendered[c], firstSamples (live.channels[c], total)) <= -120.0);
    }
}

TEST_CASE ("the offline render hears Song sample 0 and rings out the Insert Chain tails", "[export][span]")
{
    TempPackage package;
    auto mix = song::makeEmptySong (testSampleRate);
    const auto clicks = impulse (1000, 0);
    auto clicksAtBothEnds = clicks;
    clicksAtBothEnds.back() = 1.0f;
    const auto take = addTake (mix, package, "take-clicks", clicksAtBothEnds);
    mix.tracks = { trackWith ("drums", { clipOf ("clip-clicks", take, 0, 0, 1000) }) };

    // PDC latency 256 on the Track, a 700-sample echo on the Master.
    const InsertFactory inserts = [] (const song::Id& trackId, const std::vector<song::InsertSlot>&) {
        std::vector<std::unique_ptr<juce::AudioProcessor>> chain;
        if (trackId == "drums")
            chain.push_back (std::make_unique<DelayProcessor> (256));
        else
            chain.push_back (std::make_unique<EchoProcessor> (700, testSampleRate));
        return chain;
    };
    MixRender render { mix, package.root, 128, inserts };
    REQUIRE (render.latencySamples() == 256);
    REQUIRE (render.totalSamples() == 1000 + 700);
    const auto rendered = renderOffline (render, 100);

    // Clicks at 0 and 999, their echoes at 700 and 1699 (the last sample of the span).
    for (const auto& channel : rendered)
        CHECK (nonZeroSamples (channel) == std::vector<std::int64_t> { 0, 700, 999, 1699 });
    CHECK (std::abs (rendered[0][0] - centrePanGain) < 1.0e-6f);
}
