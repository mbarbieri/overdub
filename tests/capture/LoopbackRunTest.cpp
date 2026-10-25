#include "capture/LoopbackRun.h"

#include "engine/EngineFixture.h"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>

using overdub::capture::LoopbackRun;
using overdub::capture::LoopbackRunResult;
using overdub::capture::RecordLatency;
using overdub::core::ScopedLogCapture;
using overdub::engine::Engine;
using overdub::engine::EngineConfig;
using overdub::song::SampleCount;
using namespace overdub::engine::testing;

namespace
{
constexpr int blockSize = 256;

// Starts a run on the engine, drives the loopback device until the run has a result (polling it
// after every block, as the window's timer would), and returns it.
LoopbackRunResult runUntilMeasured (Engine& engine, LoopbackDevice& device, RecordLatency latency, const juce::File& parentFolder)
{
    auto started = LoopbackRun::start (engine, latency, testSampleRate, parentFolder);
    if (! started.has_value())
        FAIL ("start failed: " << started.error().code << " " << started.error().fields.dump());
    auto& run = **started;

    // The signal is 6 s; the run ends 0.75 s after it. Twice that is the most it may take.
    const auto maxBlocks = 2 * (int) (run.endSample() / blockSize);
    for (int block = 0; block < maxBlocks; ++block)
    {
        device.processOneBlock (engine);
        if (auto polled = run.poll(); polled.has_value())
        {
            if (! polled->has_value())
                FAIL ("the run failed: " << polled->error().code << " " << polled->error().fields.dump());
            return **polled;
        }
    }
    FAIL ("the run never finished");
    return {};
}

std::vector<nlohmann::json> eventsNamed (const ScopedLogCapture& capture, const std::string& name)
{
    std::vector<nlohmann::json> found;
    for (const auto& event : capture.events())
        if (event.at ("event") == name)
            found.push_back (event);
    return found;
}
} // namespace

TEST_CASE ("a loopback run measures how late Takes land on this device", "[capture][calibration][loopback]")
{
    // The real round trip is 1000 samples; the device reports 300 + 400 and the stored calibration
    // adds 100, so Takes land 200 samples late.
    TempPackage temp;
    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = blockSize } };
    LoopbackDevice device { 2, blockSize, 1000 };

    const auto result = runUntilMeasured (engine, device, RecordLatency { 300, 400, 100 }, temp.root);

    CHECK (result.measurement.signalFound);
    CHECK (result.measurement.offsetSamples == 200);
    CHECK (result.measurement.jitterSamples == 0);
    CHECK (result.measurement.lagsSamples == std::vector<SampleCount> (7, 200));
    CHECK (result.report["pass"] == true);
    CHECK (result.report["offset_samples"] == 200);
    CHECK (result.report["calibration_offset_samples"] == 100);
    CHECK (result.report["reported_latency_samples"] == 700);
    CHECK (result.report["overrun_frames"] == 0);
}

TEST_CASE ("after the measured offset is added to the calibration, Takes land where they were heard", "[capture][calibration][loopback]")
{
    TempPackage temp;
    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = blockSize } };
    LoopbackDevice device { 2, blockSize, 1000 };

    const auto before = runUntilMeasured (engine, device, RecordLatency { 300, 400, 0 }, temp.root);
    REQUIRE (before.measurement.offsetSamples == 300);

    const auto after = runUntilMeasured (engine, device, RecordLatency { 300, 400, 0 + before.measurement.offsetSamples }, temp.root);
    CHECK (after.measurement.offsetSamples == 0);
    CHECK (after.measurement.jitterSamples == 0);
}

TEST_CASE ("a loopback run leaves nothing behind: its folder is deleted and the engine stopped", "[capture][calibration][loopback]")
{
    TempPackage temp;
    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = blockSize } };
    LoopbackDevice device { 2, blockSize, 512 };

    (void) runUntilMeasured (engine, device, RecordLatency {}, temp.root);
    device.processOneBlock (engine);

    CHECK (temp.root.getNumberOfChildFiles (juce::File::findFilesAndDirectories) == 0);
    CHECK_FALSE (engine.isPlaying());
}

// Two processes calibrating at once share the temp folder (issue #187).
TEST_CASE ("two loopback runs in one folder each have a folder of their own, and ending one leaves the other's", "[capture][calibration][loopback]")
{
    TempPackage temp;
    Engine firstEngine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = blockSize } };
    Engine secondEngine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = blockSize } };
    const auto runFolders = [&temp] { return temp.root.findChildFiles (juce::File::findDirectories, false); };

    auto first = LoopbackRun::start (firstEngine, RecordLatency {}, testSampleRate, temp.root);
    REQUIRE (first.has_value());
    auto second = LoopbackRun::start (secondEngine, RecordLatency {}, testSampleRate, temp.root);
    REQUIRE (second.has_value());
    REQUIRE (runFolders().size() == 2);

    first->reset();
    const auto secondRunning = runFolders();
    REQUIRE (secondRunning.size() == 1);
    CHECK (secondRunning.getFirst().getChildFile ("Loopback.overdub").exists());
}

TEST_CASE ("a loopback run logs capture.calibration_run_started and capture.calibration_run_finished", "[capture][calibration][loopback][log]")
{
    ScopedLogCapture logs;
    TempPackage temp;
    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = blockSize } };
    LoopbackDevice device { 2, blockSize, 512 };

    (void) runUntilMeasured (engine, device, RecordLatency { 256, 256, 0 }, temp.root);

    const auto started = eventsNamed (logs, "capture.calibration_run_started");
    REQUIRE (started.size() == 1);
    CHECK (started[0]["bursts"] == 7);
    const auto finished = eventsNamed (logs, "capture.calibration_run_finished");
    REQUIRE (finished.size() == 1);
    CHECK (finished[0]["offset_samples"] == 0);
    CHECK (finished[0]["jitter_samples"] == 0);
    CHECK (finished[0]["pass"] == true);
}

TEST_CASE ("a loopback run whose device stops calling back fails with capture.calibration_timeout", "[capture][calibration][loopback]")
{
    TempPackage temp;
    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = blockSize } };
    LoopbackDevice device { 2, blockSize, 512 };
    const auto startedAt = LoopbackRun::Clock::now();
    auto started = LoopbackRun::start (engine, RecordLatency {}, testSampleRate, temp.root, startedAt);
    REQUIRE (started.has_value());
    auto& run = **started;
    device.run (engine, 10);

    // The run lasts 6.75 s; 5 s later without reaching its end it gives up.
    CHECK_FALSE (run.poll (startedAt + std::chrono::milliseconds (11'000)).has_value());
    const auto timedOut = run.poll (startedAt + std::chrono::milliseconds (12'000));
    REQUIRE (timedOut.has_value());
    REQUIRE_FALSE (timedOut->has_value());
    CHECK (timedOut->error().code == "capture.calibration_timeout");
    CHECK_FALSE (run.poll (startedAt + std::chrono::milliseconds (13'000)).has_value());

    device.run (engine, 1); // the audio thread takes the stop, so the run closes its Pass cleanly
}
