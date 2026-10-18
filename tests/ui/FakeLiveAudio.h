#pragma once

#include "ui/LiveAudio.h"

#include "engine/EngineFixture.h"
#include "engine/LatencyCalibration.h"

#include <algorithm>
#include <map>
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
    // More devices, listed for input and output, and what each offers in the settings window
    // (offerFor); a device not listed here offers offeredRates and liveBlockSize.
    std::map<std::string, DeviceOffer> offersByDevice;
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
        for (const auto& [device, offer] : machine.offersByDevice)
            for (auto* listed : { &choices.inputDevices, &choices.outputDevices })
                if (! std::ranges::contains (*listed, device))
                    listed->push_back (device);
        choices.sampleRates = machine.offeredRates;
        choices.bufferSizes = { liveBlockSize };
        choices.current = engine::DeviceSettings { .deviceType = "Fake", .inputDevice = machine.inputDevice, .outputDevice = machine.outputDevice, .sampleRate = rate, .bufferSize = liveBlockSize };
        offers = machine.offersByDevice;
        defaultOffer = DeviceOffer { machine.offeredRates, { liveBlockSize } };
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
    DeviceOffer offerFor (const std::string& inputDevice, const std::string& outputDevice) override
    {
        const auto offerOf = [&] (const std::string& device) {
            const auto found = offers.find (device);
            return found != offers.end() ? found->second : defaultOffer;
        };
        if (inputDevice == outputDevice)
            return offerOf (outputDevice);
        const auto input = offerOf (inputDevice);
        const auto output = offerOf (outputDevice);
        return { offeredByBoth (output.sampleRates, input.sampleRates), offeredByBoth (output.bufferSizes, input.bufferSizes) };
    }

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
    std::map<std::string, DeviceOffer> offers;
    DeviceOffer defaultOffer;
    std::unique_ptr<engine::Engine> audioEngine;
    engine::testing::LoopbackDevice device { 2, liveBlockSize, 512 };
};
} // namespace overdub::ui::testing
