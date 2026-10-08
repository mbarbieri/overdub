#include "engine/Engine.h"
#include "engine/EngineEvents.h"
#include "engine/RecordSession.h"

#include "EngineFixture.h"
#include "TestProcessors.h"
#include "support/RealtimeGuardCatch.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <string>
#include <vector>

using overdub::core::ScopedLogCapture;
using overdub::engine::Anchor;
using overdub::engine::Engine;
using overdub::engine::EngineConfig;
using overdub::engine::RecordInput;
using overdub::engine::RecordSession;
using overdub::engine::RecordState;
using overdub::engine::RecordStopReason;
using namespace overdub::engine::testing;

namespace
{
constexpr int blockSize = 256;

EngineConfig stereoConfig()
{
    return EngineConfig { .numOutputChannels = 2, .maxBlockSize = blockSize };
}

// A ramp that differs per channel and per sample, so any misplaced or swapped sample shows.
std::vector<float> rampFrom (float start, std::int64_t length)
{
    std::vector<float> samples ((std::size_t) length);
    for (std::size_t i = 0; i < samples.size(); ++i)
        samples[i] = start + 0.0001f * (float) i;
    return samples;
}

// Everything the session queued, per input and channel.
std::vector<std::vector<std::vector<float>>> drainAll (RecordSession& session)
{
    std::vector<std::vector<std::vector<float>>> drained;
    for (const auto& input : session.inputs())
        drained.emplace_back (input.inputChannels.size());
    session.drain ([&] (std::size_t input, const float* const* channels, int frames) {
        for (std::size_t c = 0; c < drained[input].size(); ++c)
            drained[input][c].insert (drained[input][c].end(), channels[c], channels[c] + frames);
    });
    return drained;
}

std::vector<float> slice (const std::vector<float>& samples, std::int64_t from, std::int64_t to)
{
    return { samples.begin() + from, samples.begin() + to };
}

overdub::song::Song emptySong()
{
    return overdub::song::makeEmptySong (testSampleRate);
}

std::vector<nlohmann::json> eventsNamed (const ScopedLogCapture& capture, const std::string& name)
{
    std::vector<nlohmann::json> found;
    for (const auto& event : capture.events())
        if (event.at ("event") == name)
            found.push_back (event);
    return found;
}

overdub::song::Track armedTrack (const std::string& id, std::vector<int> inputChannels)
{
    auto track = trackWith (id, {});
    track.armed = true;
    track.inputChannels = std::move (inputChannels);
    return track;
}
} // namespace

TEST_CASE ("a Pass captures every input sample from its first playing block, at that block's Song position", "[engine][record]")
{
    TempPackage package;
    Engine engine { stereoConfig() };
    engine.loadSong (emptySong(), package.root);
    engine.seek (1000);

    LoopbackDevice device { 2, blockSize, blockSize };
    device.extraInput[0] = rampFrom (0.1f, 20 * blockSize);
    device.extraInput[1] = rampFrom (-0.1f, 20 * blockSize);

    RecordSession session { { RecordInput { "track-mono", { 1 } }, RecordInput { "track-stereo", { 0, 1 } } }, testSampleRate };
    engine.startRecording (session);

    // Stopped: the Pass waits for the transport.
    device.run (engine, 2);
    CHECK (session.state() == RecordState::waiting);
    CHECK_FALSE (engine.isRecording());

    engine.play();
    device.run (engine, 5); // stream blocks 2..6 are the Pass
    CHECK (engine.isRecording());
    CHECK (session.state() == RecordState::recording);

    engine.stopRecording (session);
    device.run (engine, 2);
    CHECK (session.state() == RecordState::stopped);
    CHECK (session.stopReason() == RecordStopReason::requested);
    CHECK_FALSE (engine.isRecording());

    CHECK (session.startSongSample() == 1000);
    CHECK (session.capturedFrames() == 5 * blockSize);
    const auto drained = drainAll (session);
    const auto passStart = 2 * blockSize;
    const auto passEnd = 7 * blockSize;
    CHECK (drained[0][0] == slice (device.inputHistory[1], passStart, passEnd));
    CHECK (drained[1][0] == slice (device.inputHistory[0], passStart, passEnd));
    CHECK (drained[1][1] == slice (device.inputHistory[1], passStart, passEnd));
}

TEST_CASE ("a Pass ends when the transport stops or seeks", "[engine][record]")
{
    TempPackage package;
    Engine engine { stereoConfig() };
    engine.loadSong (emptySong(), package.root);
    LoopbackDevice device { 2, blockSize, blockSize };
    engine.play();

    RecordSession stoppedByTransport { { RecordInput { "track", { 0 } } }, testSampleRate };
    engine.startRecording (stoppedByTransport);
    device.run (engine, 3);
    engine.stop();
    device.run (engine, 1);
    CHECK (stoppedByTransport.state() == RecordState::stopped);
    CHECK (stoppedByTransport.stopReason() == RecordStopReason::transportStopped);
    CHECK (stoppedByTransport.capturedFrames() == 3 * blockSize);

    engine.play();
    RecordSession stoppedBySeek { { RecordInput { "track", { 0 } } }, testSampleRate };
    engine.startRecording (stoppedBySeek);
    device.run (engine, 2);
    engine.seek (50'000);
    device.run (engine, 1);
    CHECK (stoppedBySeek.state() == RecordState::stopped);
    CHECK (stoppedBySeek.stopReason() == RecordStopReason::seeked);
    CHECK (stoppedBySeek.capturedFrames() == 2 * blockSize);
}

TEST_CASE ("stopping a Pass the audio thread has not started cancels it", "[engine][record]")
{
    TempPackage package;
    Engine engine { stereoConfig() };
    engine.loadSong (emptySong(), package.root);
    LoopbackDevice device { 2, blockSize, blockSize };

    RecordSession session { { RecordInput { "track", { 0 } } }, testSampleRate };
    engine.startRecording (session);
    engine.stopRecording (session);
    CHECK (session.state() == RecordState::stopped);
    CHECK (session.stopReason() == RecordStopReason::cancelled);

    // The cancelled session is never taken, even once the transport plays.
    engine.play();
    device.run (engine, 2);
    CHECK (session.capturedFrames() == 0);
    CHECK_FALSE (engine.isRecording());
}

TEST_CASE ("a stop names its Pass: it never ends another, and a newer Pass replaces one still waiting", "[engine][record]")
{
    TempPackage package;
    Engine engine { stereoConfig() };
    engine.loadSong (emptySong(), package.root);
    LoopbackDevice device { 2, blockSize, blockSize };

    RecordSession replaced { { RecordInput { "track", { 0 } } }, testSampleRate };
    RecordSession active { { RecordInput { "track", { 0 } } }, testSampleRate };
    engine.startRecording (replaced);
    engine.startRecording (active);
    CHECK (replaced.state() == RecordState::stopped);
    CHECK (replaced.stopReason() == RecordStopReason::cancelled);

    engine.play();
    device.run (engine, 2);
    REQUIRE (active.state() == RecordState::recording);

    // A late stop for the replaced Pass leaves the active one recording.
    engine.stopRecording (replaced);
    device.run (engine, 2);
    CHECK (active.state() == RecordState::recording);
    CHECK (active.capturedFrames() == 4 * blockSize);

    engine.stopRecording (active);
    device.run (engine, 1);
    CHECK (active.state() == RecordState::stopped);
    CHECK (active.stopReason() == RecordStopReason::requested);
}

TEST_CASE ("every block of a Pass records its anchor", "[engine][record][anchor]")
{
    TempPackage package;
    Engine engine { stereoConfig() };
    engine.loadSong (emptySong(), package.root);
    LoopbackDevice device { 2, blockSize, blockSize };
    device.hostTimeNs = 5'000'000'000;
    engine.play();
    device.run (engine, 2); // Song samples 0..511 before the Pass

    RecordSession session { { RecordInput { "track", { 0 } } }, testSampleRate };
    engine.startRecording (session);
    const auto firstHostNs = device.hostTimeNs;
    device.run (engine, 3);
    engine.stopRecording (session);
    device.run (engine, 1);

    std::vector<Anchor> anchors;
    session.drainAnchors (anchors);
    const auto blockNs = device.blockDurationNs();
    CHECK (anchors == std::vector<Anchor> { { 512, firstHostNs }, { 768, firstHostNs + blockNs }, { 1024, firstHostNs + 2 * blockNs } });
}

TEST_CASE ("input that does not fit the FIFO becomes a gap of silence and later input keeps its position", "[engine][record]")
{
    TempPackage package;
    Engine engine { stereoConfig() };
    engine.loadSong (emptySong(), package.root);
    LoopbackDevice device { 2, blockSize, blockSize };
    device.extraInput[0] = rampFrom (0.2f, 10 * blockSize);
    engine.play();

    // Room for two blocks: the third and fourth do not fit until the reader drains.
    RecordSession session { { RecordInput { "track", { 0 } } }, 2 * blockSize };
    engine.startRecording (session);
    device.run (engine, 4);

    auto drained = drainAll (session);
    device.run (engine, 2);
    const auto rest = drainAll (session);
    drained[0][0].insert (drained[0][0].end(), rest[0][0].begin(), rest[0][0].end());

    CHECK (session.overrunFrames() == 2 * blockSize);
    CHECK (session.capturedFrames() == 6 * blockSize);
    REQUIRE (drained[0][0].size() == (std::size_t) (6 * blockSize));
    auto expected = slice (device.inputHistory[0], 0, 6 * blockSize);
    std::fill (expected.begin() + 2 * blockSize, expected.begin() + 4 * blockSize, 0.0f);
    CHECK (drained[0][0] == expected);
}

TEST_CASE ("recording RT events carry the block's Song position and host time", "[engine][record][rt][events]")
{
    ScopedLogCapture capture;
    TempPackage package;
    Engine engine { stereoConfig() };
    engine.loadSong (emptySong(), package.root);
    LoopbackDevice device { 2, blockSize, blockSize };
    engine.play();
    device.run (engine, 1);

    RecordSession session { { RecordInput { "track-a", { 0 } }, RecordInput { "track-b", { 1 } } }, testSampleRate };
    engine.startRecording (session);
    const auto startHostNs = device.hostTimeNs;
    device.run (engine, 2);
    engine.stopRecording (session);
    const auto stopHostNs = device.hostTimeNs;
    device.run (engine, 1);
    overdub::core::drainRtEvents (engine.rtEvents(), overdub::engine::engineRtCatalog);

    const auto started = eventsNamed (capture, "engine.recording_started");
    REQUIRE (started.size() == 1);
    CHECK (started[0]["sample"] == 256);
    CHECK (started[0]["host_ns"] == startHostNs);
    CHECK (started[0]["inputs"] == 2.0);
    CHECK (started[0]["plan_latency_samples"] == 0.0);

    const auto stopped = eventsNamed (capture, "engine.recording_stopped");
    REQUIRE (stopped.size() == 1);
    CHECK (stopped[0]["sample"] == 768);
    CHECK (stopped[0]["host_ns"] == stopHostNs);
    CHECK (stopped[0]["frames"] == 512.0);
    CHECK (stopped[0]["overrun_frames"] == 0.0);
    CHECK (stopped[0]["reason"] == (double) RecordStopReason::requested);
}

TEST_CASE ("process() neither allocates nor locks while recording and Monitoring", "[engine][record][rt]")
{
    TempPackage package;
    auto song = emptySong();
    const auto take = addTake (song, package, "take-a", sine (8000, 220.0));
    song.tracks.push_back (trackWith ("track-backing", { clipOf ("clip-a", take, 0, 0, 8000) }));
    song.tracks.push_back (armedTrack ("track-armed", { 0, 1 }));

    Engine engine { stereoConfig() };
    engine.loadSong (song, package.root, insertOnTrack ("track-backing", [] { return std::make_unique<DelayProcessor> (64); }));
    engine.setMonitoring (true);
    LoopbackDevice device { 2, blockSize, 300 };
    device.run (engine, 1);

    RecordSession session { { RecordInput { "track-armed", { 0, 1 } } }, 2 * blockSize };
    engine.startRecording (session);
    engine.play();
    // The first recording block takes the session; later ones queue input until the FIFO is full
    // and then record gaps. Both paths are guarded.
    for (int block = 0; block < 6; ++block)
    {
        device.prepareInput();
        REQUIRE_REALTIME_SAFE (device.processPreparedBlock (engine));
        device.keepHistory();
    }
    engine.stopRecording (session);
    device.prepareInput();
    REQUIRE_REALTIME_SAFE (device.processPreparedBlock (engine));
    CHECK (session.state() == RecordState::stopped);
    CHECK (session.overrunFrames() > 0);
}

TEST_CASE ("with Monitoring on, an Armed Track hears its live input; otherwise it does not", "[engine][monitoring]")
{
    TempPackage package;
    auto song = emptySong();
    song.tracks.push_back (armedTrack ("track-mono", { 1 }));
    auto unarmed = trackWith ("track-unarmed", {});
    unarmed.inputChannels = { 0 };
    song.tracks.push_back (unarmed);

    Engine engine { stereoConfig() };
    engine.loadSong (song, package.root);

    // Input channel 0 (only the unarmed Track's) has an impulse at 100, channel 1 at 300.
    LoopbackDevice device { 2, blockSize, 4 * blockSize };
    device.extraInput[0] = impulse (2 * blockSize, 100, 0.5f);
    device.extraInput[1] = impulse (2 * blockSize, 300, 0.25f);

    engine.setMonitoring (true);
    device.run (engine, 2); // transport stopped: Monitoring still sounds
    CHECK (nonZeroSamples (device.outputHistory[0]) == std::vector<std::int64_t> { 300 });
    CHECK (nonZeroSamples (device.outputHistory[1]) == std::vector<std::int64_t> { 300 });
    CHECK_THAT (device.outputHistory[0][300], Catch::Matchers::WithinULP (0.25f * centrePanGain, 0));
    CHECK_THAT (device.outputHistory[1][300], Catch::Matchers::WithinULP (0.25f * centrePanGain, 0));

    engine.setMonitoring (false);
    device.extraInput[1] = impulse (4 * blockSize, 3 * blockSize, 0.25f);
    device.run (engine, 2);
    CHECK (nonZeroSamples (slice (device.outputHistory[0], 2 * blockSize, 4 * blockSize)).empty());
}

TEST_CASE ("a stereo Armed Track monitors its first input on the left and its second on the right", "[engine][monitoring]")
{
    TempPackage package;
    auto song = emptySong();
    song.tracks.push_back (armedTrack ("track-stereo", { 1, 0 }));

    Engine engine { stereoConfig() };
    engine.loadSong (song, package.root);
    engine.setMonitoring (true);

    LoopbackDevice device { 2, blockSize, 4 * blockSize };
    device.extraInput[0] = impulse (blockSize, 40, 0.5f);
    device.extraInput[1] = impulse (blockSize, 80, 0.25f);
    device.run (engine, 1);

    CHECK (nonZeroSamples (device.outputHistory[0]) == std::vector<std::int64_t> { 80 });
    CHECK (nonZeroSamples (device.outputHistory[1]) == std::vector<std::int64_t> { 40 });
}
