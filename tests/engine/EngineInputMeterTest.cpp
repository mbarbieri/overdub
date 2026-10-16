#include "engine/Engine.h"
#include "engine/RecordSession.h"

#include "EngineFixture.h"
#include "TestProcessors.h"
#include "support/RealtimeGuardCatch.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <vector>

// Each Armed Track's input meter (issue #131): the raw peak on the device input channels it
// records, taken before its Insert Chain and fader, polled with the strips' peaks.
using overdub::engine::Engine;
using overdub::engine::EngineConfig;
using overdub::engine::RecordInput;
using overdub::engine::RecordSession;
using namespace overdub::engine::testing;
using Catch::Matchers::WithinULP;

namespace
{
constexpr int blockSize = 256;
// Long enough that no output echoes back into the input during a test.
constexpr std::int64_t noEcho = 64 * blockSize;

EngineConfig stereoConfig()
{
    return EngineConfig { .numOutputChannels = 2, .maxBlockSize = blockSize };
}

// Appends an empty Track, Armed on these input channels.
overdub::song::Track& addArmedTrack (overdub::song::Song& song, const std::string& id, std::vector<int> inputChannels)
{
    song.settings.trackInputs[id] = overdub::song::TrackInput { .armed = true, .inputChannels = std::move (inputChannels) };
    return song.tracks.emplace_back (trackWith (id, {}));
}

float peakOf (const std::vector<float>& samples, std::size_t from, std::size_t to)
{
    float peak = 0.0f;
    for (auto i = from; i < to; ++i)
        peak = std::max (peak, std::abs (samples[i]));
    return peak;
}

// Guitar is Armed on input 1, turned down, with a gain Insert; Stereo is Armed on inputs 0 and 1;
// Bass records input 0 but is not Armed.
overdub::song::Song meteredSong()
{
    auto song = overdub::song::makeEmptySong (testSampleRate);
    addArmedTrack (song, "guitar", { 1 }).volumeDb = -20.0;
    addArmedTrack (song, "stereo", { 0, 1 });
    song.tracks.push_back (trackWith ("bass", {}));
    song.settings.trackInputs["bass"] = overdub::song::TrackInput { .inputChannels = { 0 } };
    return song;
}
} // namespace

TEST_CASE ("an Armed Track's input meter reports the peak on its input channels, whatever its volume, Insert Chain or Monitoring", "[engine][meters][input_meter]")
{
    TempPackage package;
    const auto song = meteredSong();
    const auto halfGain = insertOnTrack ("guitar", [] { return std::make_unique<GainProcessor> (0.5f); });

    const auto onInput0 = sine (4 * blockSize, 1000.0, 0.3f);
    const auto onInput1 = sine (4 * blockSize, 1000.0, 0.6f);
    const auto peak0 = peakOf (onInput0, 0, onInput0.size());
    const auto peak1 = peakOf (onInput1, 0, onInput1.size());
    REQUIRE (peak1 > peak0);

    for (const auto monitoringOn : { false, true })
    {
        DYNAMIC_SECTION ("Monitoring " << (monitoringOn ? "on" : "off"))
        {
            Engine engine { stereoConfig() };
            engine.loadSong (song, package.root, halfGain, monitoringOn ? std::set<overdub::song::Id> { "guitar", "stereo" } : std::set<overdub::song::Id> {});
            engine.setMonitoring (monitoringOn);
            LoopbackDevice device { 2, blockSize, noEcho };
            device.extraInput[0] = onInput0;
            device.extraInput[1] = onInput1;
            device.run (engine, 4);

            const auto peaks = engine.takeMeterPeaks();
            CHECK (peaks.inputs.size() == 2);
            CHECK_THAT (peaks.inputs.at ("guitar"), WithinULP (peak1, 0));
            CHECK_THAT (peaks.inputs.at ("stereo"), WithinULP (peak1, 0));
            CHECK (! peaks.inputs.contains ("bass"));
            // The strip meters still read post-fader: the guitar strip hears its input only when
            // monitored, through the gain Insert and the -20 dB fader.
            const auto heardGuitar = monitoringOn ? peak1 * 0.5f * (float) std::pow (10.0, -20.0 / 20.0) * centrePanGain : 0.0f;
            CHECK_THAT (peaks.tracks.at ("guitar"), Catch::Matchers::WithinRel (heardGuitar, 1.0e-5f));
        }
    }
}

TEST_CASE ("an Armed Track's input meter reads its input while a Pass records it", "[engine][meters][input_meter][record]")
{
    TempPackage package;
    auto song = overdub::song::makeEmptySong (testSampleRate);
    addArmedTrack (song, "guitar", { 1 });
    const auto onInput1 = sine (4 * blockSize, 1000.0, 0.45f);

    Engine engine { stereoConfig() };
    engine.loadSong (song, package.root);
    LoopbackDevice device { 2, blockSize, noEcho };
    device.extraInput[1] = onInput1;
    RecordSession session { { RecordInput { "guitar", { 1 } } }, testSampleRate };
    engine.startRecording (session);
    engine.play();
    device.run (engine, 4);

    REQUIRE (engine.isRecording());
    CHECK_THAT (engine.takeMeterPeaks().inputs.at ("guitar"), WithinULP (peakOf (onInput1, 0, onInput1.size()), 0));
    engine.stopRecording (session);
    device.run (engine, 1);
}

TEST_CASE ("each poll's input peak covers only the blocks since the previous poll", "[engine][meters][input_meter]")
{
    TempPackage package;
    Engine engine { stereoConfig() };
    engine.loadSong (meteredSong(), package.root);
    LoopbackDevice device { 2, blockSize, noEcho };

    // Loud for two blocks, then quiet for two.
    auto signal = sine (4 * blockSize, 1000.0, 0.8f);
    const auto quiet = sine (4 * blockSize, 1000.0, 0.1f);
    std::copy (quiet.begin() + 2 * blockSize, quiet.end(), signal.begin() + 2 * blockSize);
    device.extraInput[1] = signal;

    device.run (engine, 2);
    CHECK_THAT (engine.takeMeterPeaks().inputs.at ("guitar"), WithinULP (peakOf (signal, 0, 2 * blockSize), 0));
    device.run (engine, 2);
    CHECK_THAT (engine.takeMeterPeaks().inputs.at ("guitar"), WithinULP (peakOf (signal, 2 * blockSize, 4 * blockSize), 0));
    CHECK_THAT (engine.takeMeterPeaks().inputs.at ("guitar"), WithinULP (0.0f, 0));
}

TEST_CASE ("process() neither allocates nor locks while it meters Armed Tracks' input", "[engine][meters][input_meter][rt]")
{
    TempPackage package;
    Engine engine { stereoConfig() };
    engine.loadSong (meteredSong(), package.root);
    LoopbackDevice device { 2, blockSize, noEcho };
    device.extraInput[0] = sine (4 * blockSize, 440.0, 0.5f);
    device.extraInput[1] = sine (4 * blockSize, 880.0, 0.7f);
    device.run (engine, 1);

    for (int block = 0; block < 3; ++block)
    {
        device.prepareInput();
        REQUIRE_REALTIME_SAFE (device.processPreparedBlock (engine));
        device.keepHistory();
    }
    const auto expected = std::max (peakOf (device.extraInput[0], 0, 4 * blockSize), peakOf (device.extraInput[1], 0, 4 * blockSize));
    CHECK_THAT (engine.takeMeterPeaks().inputs.at ("stereo"), WithinULP (expected, 0));
}

TEST_CASE ("only the monitored Armed Tracks hear their input", "[engine][monitoring]")
{
    TempPackage package;
    auto song = overdub::song::makeEmptySong (testSampleRate);
    addArmedTrack (song, "guitar", { 0 });
    addArmedTrack (song, "vocals", { 1 });

    Engine engine { stereoConfig() };
    engine.loadSong (song, package.root, {}, std::set<overdub::song::Id> { "vocals" });
    engine.setMonitoring (true);
    LoopbackDevice device { 2, blockSize, noEcho };
    device.extraInput[0] = impulse (blockSize, 100, 0.5f);
    device.extraInput[1] = impulse (blockSize, 200, 0.25f);
    device.run (engine, 1);

    CHECK (nonZeroSamples (device.outputHistory[0]) == std::vector<std::int64_t> { 200 });
    CHECK_THAT (device.outputHistory[0][200], WithinULP (0.25f * centrePanGain, 0));
}
