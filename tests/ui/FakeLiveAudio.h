#pragma once

#include "ui/LiveAudio.h"

#include "engine/EngineFixture.h"
#include "engine/LatencyCalibration.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// The test adapter for ui::LiveAudio (issue #105): an Engine that a loopback fake device drives,
// opened on a FakeMachine that says which device is plugged in and which rates it offers. Like
// app::LiveEngine, it reads the device's stored offset from the latency calibration file when it
// opens, and switches to the Song's rate when the device offers it. Nothing opens audio hardware.
namespace overdub::ui::testing
{
inline constexpr int liveBlockSize = 256;

// What the machine offers each LiveAudio opened on it; the test changes it between opens.
struct FakeMachine
{
    bool deviceAvailable = true;
    std::string inputDevice = "Scarlett 2i2 USB";
    std::string outputDevice = "Scarlett 2i2 USB";
    int deviceRate = engine::testing::testSampleRate;
    std::vector<int> offeredRates { engine::testing::testSampleRate };
    juce::File latencyCalibrationFile;
};

class FakeLiveAudio final : public LiveAudio
{
public:
    FakeLiveAudio (const FakeMachine& machine, std::optional<int> songSampleRate)
    {
        if (! machine.deviceAvailable)
            return;
        rate = machine.deviceRate;
        if (songSampleRate.has_value() && std::ranges::contains (machine.offeredRates, *songSampleRate))
            rate = *songSampleRate;
        key = { machine.inputDevice, machine.outputDevice, rate };
        choices.deviceType = "Fake";
        choices.inputDevices = { machine.inputDevice };
        choices.outputDevices = { machine.outputDevice };
        choices.sampleRates = machine.offeredRates;
        choices.bufferSizes = { liveBlockSize };
        choices.current = engine::DeviceSettings { .deviceType = "Fake", .inputDevice = machine.inputDevice, .outputDevice = machine.outputDevice, .sampleRate = rate, .bufferSize = liveBlockSize };
        stored = engine::findCalibrationOffset (machine.latencyCalibrationFile, key).value_or (std::nullopt);
        latency.calibrationOffsetSamples = stored.value_or (0);
        audioEngine = std::make_unique<engine::Engine> (engine::EngineConfig { .numOutputChannels = 2, .maxBlockSize = liveBlockSize });
    }

    engine::Engine* engine() override { return audioEngine.get(); }
    AudioBinding audioBinding() override
    {
        AudioBinding binding { audioEngine.get(), latency, {} };
        if (audioEngine == nullptr)
            return binding;
        binding.inputChannelNames = { "In 1", "In 2" };
        binding.outputDeviceName = key.outputDevice;
        binding.sampleRate = rate;
        binding.offeredSampleRates = choices.sampleRates;
        return binding;
    }
    capture::RecordLatency recordLatency() const override { return latency; }
    const engine::CalibrationKey& calibrationKey() const override { return key; }
    std::optional<song::SampleCount> storedCalibrationOffset() const override { return stored; }
    void setCalibrationOffset (song::SampleCount offsetSamples) override
    {
        stored = offsetSamples;
        latency.calibrationOffsetSamples = offsetSamples;
    }
    int sampleRate() const override { return rate; }
    DeviceChoices deviceChoices() override { return choices; }

    // Runs device callbacks, as the device's audio thread would.
    void run (int blocks)
    {
        REQUIRE (audioEngine != nullptr);
        device.run (*audioEngine, blocks);
    }

    // The device reports a change (it stopped calling back first when it was lost).
    void reportChange (engine::DeviceChange change)
    {
        if (change == engine::DeviceChange::lost && audioEngine != nullptr)
            audioEngine->deviceStopped();
        REQUIRE (onDeviceChange != nullptr);
        onDeviceChange (change);
    }

private:
    int rate = 0;
    engine::CalibrationKey key;
    std::optional<song::SampleCount> stored;
    capture::RecordLatency latency;
    DeviceChoices choices;
    std::unique_ptr<engine::Engine> audioEngine;
    engine::testing::LoopbackDevice device { 2, liveBlockSize, 512 };
};
} // namespace overdub::ui::testing
