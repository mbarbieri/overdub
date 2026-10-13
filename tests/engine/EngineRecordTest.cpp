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

TEST_CASE ("a Pass with a record point begins at the block that plays it and captures from that sample on", "[engine][record][pre_roll]")
{
    TempPackage package;
    Engine engine { stereoConfig() };
    engine.loadSong (emptySong(), package.root);
    engine.seek (1000);

    LoopbackDevice device { 2, blockSize, blockSize };
    device.extraInput[0] = rampFrom (0.1f, 20 * blockSize);

    // The record point falls 100 samples into the third block.
    constexpr std::int64_t recordPoint = 1000 + 2 * blockSize + 100;
    RecordSession session { { RecordInput { "track", { 0 } } }, testSampleRate };
    session.startAt (recordPoint);
    engine.startRecording (session);
    engine.play();

    device.run (engine, 2);
    CHECK (session.state() == RecordState::waiting);
    CHECK_FALSE (engine.isRecording());

    device.run (engine, 3);
    CHECK (engine.isRecording());
    CHECK (session.startSongSample() == recordPoint);
    CHECK (session.capturedFrames() == 5 * blockSize - (2 * blockSize + 100));

    engine.stopRecording (session);
    device.run (engine, 1);
    CHECK (session.stopReason() == RecordStopReason::requested);
    CHECK (drainAll (session)[0][0] == slice (device.inputHistory[0], 2 * blockSize + 100, 5 * blockSize));
}

TEST_CASE ("a Pass waiting for its record point ends like a running one, having captured nothing", "[engine][record][pre_roll]")
{
    TempPackage package;
    Engine engine { stereoConfig() };
    engine.loadSong (emptySong(), package.root);
    LoopbackDevice device { 2, blockSize, blockSize };
    engine.play();

    RecordSession stopped { { RecordInput { "track", { 0 } } }, testSampleRate };
    stopped.startAt (100'000);
    engine.startRecording (stopped);
    device.run (engine, 2);
    engine.stopRecording (stopped);
    device.run (engine, 1);
    CHECK (stopped.state() == RecordState::stopped);
    CHECK (stopped.stopReason() == RecordStopReason::requested);
    CHECK (stopped.capturedFrames() == 0);

    RecordSession byTransport { { RecordInput { "track", { 0 } } }, testSampleRate };
    byTransport.startAt (100'000);
    engine.startRecording (byTransport);
    device.run (engine, 2);
    engine.stop();
    device.run (engine, 1);
    CHECK (byTransport.stopReason() == RecordStopReason::transportStopped);
    CHECK (byTransport.capturedFrames() == 0);
    CHECK_FALSE (engine.isRecording());
}

TEST_CASE ("a record point the transport has already passed starts the Pass at once", "[engine][record][pre_roll]")
{
    TempPackage package;
    Engine engine { stereoConfig() };
    engine.loadSong (emptySong(), package.root);
    engine.seek (5000);
    LoopbackDevice device { 2, blockSize, blockSize };

    RecordSession session { { RecordInput { "track", { 0 } } }, testSampleRate };
    session.startAt (1000);
    engine.startRecording (session);
    engine.play();
    device.run (engine, 2);
    CHECK (session.startSongSample() == 5000);
    CHECK (session.capturedFrames() == 2 * blockSize);
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

TEST_CASE ("process() neither allocates nor locks while a Pass waits for its record point and begins mid-block", "[engine][record][rt][pre_roll]")
{
    TempPackage package;
    auto song = emptySong();
    song.tracks.push_back (armedTrack ("track-armed", { 0 }));
    Engine engine { stereoConfig() };
    engine.loadSong (song, package.root);
    LoopbackDevice device { 2, blockSize, blockSize };
    device.run (engine, 1);

    RecordSession session { { RecordInput { "track-armed", { 0 } } }, 8 * blockSize };
    session.startAt (2 * blockSize + 50);
    engine.startRecording (session);
    engine.play();
    for (int block = 0; block < 5; ++block)
    {
        device.prepareInput();
        REQUIRE_REALTIME_SAFE (device.processPreparedBlock (engine));
        device.keepHistory();
    }
    CHECK (session.startSongSample() == 2 * blockSize + 50);
    CHECK (session.capturedFrames() == 5 * blockSize - (2 * blockSize + 50));
}

TEST_CASE ("Monitoring goes through the Armed Track's Insert Chain while its Take stays dry", "[engine][monitoring][record][inserts]")
{
    TempPackage package;
    auto song = emptySong();
    song.tracks.push_back (armedTrack ("track-guitar", { 1 }));
    static constexpr int pdc = 64;

    Engine engine { stereoConfig() };
    // Gain and a delaying processor on the Armed Track: Monitoring is heard through both.
    engine.loadSong (song, package.root, [] (const overdub::song::Id& trackId, const std::vector<overdub::song::InsertSlot>&) {
        std::vector<std::unique_ptr<juce::AudioProcessor>> inserts;
        if (trackId == "track-guitar")
        {
            inserts.push_back (std::make_unique<GainProcessor> (0.5f));
            inserts.push_back (std::make_unique<DelayProcessor> (pdc));
        }
        return inserts;
    });

    LoopbackDevice device { 2, blockSize, 8 * blockSize };
    device.extraInput[1] = impulse (8 * blockSize, 3 * blockSize + 10, 0.8f);
    engine.setMonitoring (true);
    RecordSession session { { RecordInput { "track-guitar", { 1 } } }, testSampleRate };
    engine.startRecording (session);
    engine.play();
    device.run (engine, 6);
    engine.stopRecording (session);
    device.run (engine, 1);

    // Heard through the chain: halved and delayed by the chain's latency.
    CHECK (nonZeroSamples (device.outputHistory[0]) == std::vector<std::int64_t> { 3 * blockSize + 10 + pdc });
    CHECK_THAT (device.outputHistory[0][3 * blockSize + 10 + pdc], Catch::Matchers::WithinULP (0.4f * centrePanGain, 0));
    // Captured dry: the input exactly as it came in.
    const auto drained = drainAll (session);
    CHECK (drained[0][0] == slice (device.inputHistory[1], 0, 6 * blockSize));
}

TEST_CASE ("a device that stops ends the running Pass where it stopped, keeping what it captured, and stops the transport", "[engine][record][device_change]")
{
    ScopedLogCapture capture;
    TempPackage package;
    Engine engine { stereoConfig() };
    engine.loadSong (emptySong(), package.root);
    LoopbackDevice device { 2, blockSize, blockSize };
    device.extraInput[0] = rampFrom (0.1f, 20 * blockSize);
    engine.play();

    RecordSession session { { RecordInput { "track", { 0 } } }, testSampleRate };
    engine.startRecording (session);
    device.run (engine, 4);
    REQUIRE (engine.isRecording());

    // The device is gone: no callback runs from here on.
    engine.deviceStopped();
    CHECK (session.state() == RecordState::stopped);
    CHECK (session.stopReason() == RecordStopReason::deviceStopped);
    CHECK (session.capturedFrames() == 4 * blockSize);
    CHECK (drainAll (session)[0][0] == slice (device.inputHistory[0], 0, 4 * blockSize));
    CHECK_FALSE (engine.isRecording());
    CHECK_FALSE (engine.isPlaying());
    CHECK (engine.position() == 4 * blockSize);

    const auto stopped = eventsNamed (capture, "engine.device_stopped");
    REQUIRE (stopped.size() == 1);
    CHECK (stopped[0]["lvl"] == "info");
    CHECK (stopped[0]["was_playing"] == true);
    CHECK (stopped[0]["pass_ended"] == true);
    CHECK (stopped[0]["captured_frames"] == 4 * blockSize);
    CHECK (stopped[0]["position"] == 4 * blockSize);

    // Another device that starts calling the engine finds it stopped, recording nothing.
    device.run (engine, 2);
    CHECK_FALSE (engine.isPlaying());
    CHECK (session.capturedFrames() == 4 * blockSize);
}

TEST_CASE ("a Pass the audio thread had not taken when the device stopped ends having captured nothing", "[engine][record][device_change]")
{
    TempPackage package;
    Engine engine { stereoConfig() };
    engine.loadSong (emptySong(), package.root);
    LoopbackDevice device { 2, blockSize, blockSize };

    RecordSession session { { RecordInput { "track", { 0 } } }, testSampleRate };
    engine.startRecording (session);
    engine.play();
    engine.deviceStopped();
    CHECK (session.state() == RecordState::stopped);
    CHECK (session.stopReason() == RecordStopReason::deviceStopped);

    // Never taken afterwards.
    engine.play();
    device.run (engine, 2);
    CHECK (session.capturedFrames() == 0);
    CHECK_FALSE (engine.isRecording());
}

TEST_CASE ("a device that stops while idle leaves the engine stopped where it was", "[engine][device_change]")
{
    ScopedLogCapture capture;
    TempPackage package;
    Engine engine { stereoConfig() };
    engine.loadSong (emptySong(), package.root);
    engine.seek (5000);
    LoopbackDevice device { 2, blockSize, blockSize };
    device.run (engine, 1);

    engine.deviceStopped();
    CHECK_FALSE (engine.isPlaying());
    CHECK (engine.position() == 5000);
    const auto stopped = eventsNamed (capture, "engine.device_stopped");
    REQUIRE (stopped.size() == 1);
    CHECK (stopped[0]["was_playing"] == false);
    CHECK (stopped[0]["pass_ended"] == false);
}
