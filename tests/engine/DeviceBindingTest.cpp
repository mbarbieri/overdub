#include "engine/DeviceBinding.h"
#include "engine/Engine.h"

#include "EngineFixture.h"
#include "FakeAudioIODevice.h"
#include "support/RealtimeGuardCatch.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using overdub::core::ScopedLogCapture;
using overdub::engine::DeviceBinding;
using overdub::engine::Engine;
using overdub::engine::EngineConfig;
using namespace overdub::engine::testing;

namespace
{
std::vector<nlohmann::json> eventsNamed (const ScopedLogCapture& capture, const std::string& name)
{
    std::vector<nlohmann::json> found;
    for (const auto& event : capture.events())
        if (event.at ("event") == name)
            found.push_back (event);
    return found;
}

overdub::song::Song impulseSong (const TempPackage& package, std::int64_t impulseAt)
{
    auto song = overdub::song::makeEmptySong (testSampleRate);
    const auto take = addTake (song, package, "take-a", impulse (2000, impulseAt));
    song.tracks.push_back (trackWith ("track-a", { clipOf ("clip-a", take, 0, 0, 2000) }));
    return song;
}

// Device-shaped buffers for calling the binding's callback directly, as a device would.
struct FakeDeviceBuffers
{
    std::vector<std::vector<float>> inputStorage;
    std::vector<std::vector<float>> outputStorage;
    std::vector<const float*> inputPointers;
    std::vector<float*> outputPointers;
    int blockSize;

    FakeDeviceBuffers (int inputs, int outputs, int samplesPerBlock)
        : inputStorage ((std::size_t) inputs, std::vector<float> ((std::size_t) samplesPerBlock, 0.25f)),
          outputStorage ((std::size_t) outputs, std::vector<float> ((std::size_t) samplesPerBlock, 0.5f)),
          blockSize (samplesPerBlock)
    {
        for (auto& channel : inputStorage)
            inputPointers.push_back (channel.data());
        for (auto& channel : outputStorage)
            outputPointers.push_back (channel.data());
    }

    void callback (DeviceBinding& binding, const juce::AudioIODeviceCallbackContext& context)
    {
        binding.audioDeviceIOCallbackWithContext (inputPointers.data(),
                                                  (int) inputPointers.size(),
                                                  outputPointers.data(),
                                                  (int) outputPointers.size(),
                                                  blockSize,
                                                  context);
    }
};
} // namespace

TEST_CASE ("the device callback plays the engine into the device's outputs at the device's host time", "[engine][device][binding]")
{
    TempPackage package;
    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = 256 } };
    engine.loadSong (impulseSong (package, 300), package.root);
    engine.play();
    DeviceBinding binding { engine };
    FakeDeviceBuffers device { 1, 2, 256 };

    const std::uint64_t firstHostNs = 5'000'000'000;
    const std::uint64_t secondHostNs = 5'005'333'333;
    device.callback (binding, { &firstHostNs });
    CHECK (nonZeroSamples (device.outputStorage[0]).empty());
    device.callback (binding, { &secondHostNs });

    CHECK (nonZeroSamples (device.outputStorage[0]) == std::vector<std::int64_t> { 300 - 256 });
    CHECK (nonZeroSamples (device.outputStorage[1]) == std::vector<std::int64_t> { 300 - 256 });
    REQUIRE (engine.latestAnchor().has_value());
    CHECK (*engine.latestAnchor() == overdub::engine::Anchor { 256, secondHostNs });
}

TEST_CASE ("a device callback without a host time is anchored at the time it ran", "[engine][device][binding]")
{
    TempPackage package;
    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = 256 } };
    engine.loadSong (impulseSong (package, 10), package.root);
    DeviceBinding binding { engine };
    FakeDeviceBuffers device { 0, 2, 256 };

    const auto beforeNs = overdub::core::hostTimeNowNs();
    device.callback (binding, juce::AudioIODeviceCallbackContext {});
    const auto afterNs = overdub::core::hostTimeNowNs();

    REQUIRE (engine.latestAnchor().has_value());
    CHECK (engine.latestAnchor()->hostTimeNs >= beforeNs);
    CHECK (engine.latestAnchor()->hostTimeNs <= afterNs);
}

TEST_CASE ("the device callback marks its thread as the audio thread", "[engine][device][binding]")
{
    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = 256 } };
    DeviceBinding binding { engine };
    FakeDeviceBuffers device { 0, 2, 256 };
    const overdub::core::ScopedThreadRole restoreRole { overdub::core::ThreadRole::worker };

    device.callback (binding, juce::AudioIODeviceCallbackContext {});

    CHECK (overdub::core::currentThreadRole() == overdub::core::ThreadRole::audio);
}

TEST_CASE ("the device callback neither allocates nor locks, with or without a host time", "[engine][device][binding][rt]")
{
    TempPackage package;
    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = 256 } };
    engine.loadSong (impulseSong (package, 300), package.root);
    DeviceBinding binding { engine };
    FakeDeviceBuffers device { 2, 2, 256 };
    const overdub::core::ScopedThreadRole restoreRole { overdub::core::ThreadRole::worker };
    const std::uint64_t hostNs = 1'000'000'000;
    const juce::AudioIODeviceCallbackContext withHostTime { &hostNs };
    const juce::AudioIODeviceCallbackContext withoutHostTime {};

    // The first callback takes the plan and starts playing, so it is guarded too.
    engine.play();
    REQUIRE_REALTIME_SAFE (device.callback (binding, withHostTime));
    REQUIRE_REALTIME_SAFE (for (int block = 0; block < 4; ++block) device.callback (binding, withoutHostTime));
}

TEST_CASE ("starting the device logs engine.device_opened with the device's shape and latencies", "[engine][device][binding]")
{
    ScopedLogCapture capture;
    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = 256 } };
    DeviceBinding binding { engine };
    FakeAudioIODevice device { FakeDeviceShape {
        .name = "Scarlett 2i2 USB",
        .typeName = "CoreAudio",
        .numInputChannels = 2,
        .numOutputChannels = 2,
        .sampleRate = 48000.0,
        .bufferSize = 256,
        .inputLatencySamples = 301,
        .outputLatencySamples = 287,
    } };

    binding.audioDeviceAboutToStart (&device);

    const auto opened = eventsNamed (capture, "engine.device_opened");
    REQUIRE (opened.size() == 1);
    CHECK (opened[0]["lvl"] == "info");
    CHECK (opened[0]["device"] == "Scarlett 2i2 USB");
    CHECK (opened[0]["type"] == "CoreAudio");
    CHECK (opened[0]["sample_rate"] == 48000);
    CHECK (opened[0]["buffer_size"] == 256);
    CHECK (opened[0]["input_channels"] == 2);
    CHECK (opened[0]["output_channels"] == 2);
    CHECK (opened[0]["input_latency_samples"] == 301);
    CHECK (opened[0]["output_latency_samples"] == 287);
    CHECK (opened[0]["calibration_offset_samples"] == 0);
    CHECK (eventsNamed (capture, "engine.device_engine_mismatch").empty());
}

TEST_CASE ("stopping the device logs engine.device_closed with the device's xrun count", "[engine][device][binding]")
{
    ScopedLogCapture capture;
    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = 256 } };
    DeviceBinding binding { engine };
    FakeAudioIODevice device { FakeDeviceShape { .name = "Scarlett 2i2 USB", .typeName = "CoreAudio" } };

    binding.audioDeviceAboutToStart (&device);
    device.shape.xruns = 3;
    binding.audioDeviceStopped();

    const auto closed = eventsNamed (capture, "engine.device_closed");
    REQUIRE (closed.size() == 1);
    CHECK (closed[0]["lvl"] == "info");
    CHECK (closed[0]["device"] == "Scarlett 2i2 USB");
    CHECK (closed[0]["type"] == "CoreAudio");
    CHECK (closed[0]["xruns"] == 3);
}

TEST_CASE ("an engine for a device has the device's output channels and buffer size", "[engine][device][binding]")
{
    FakeAudioIODevice device { FakeDeviceShape { .numInputChannels = 2, .numOutputChannels = 4, .bufferSize = 128 } };

    const auto config = overdub::engine::engineConfigFor (device);

    CHECK (config.numOutputChannels == 4);
    CHECK (config.maxBlockSize == 128);
}

TEST_CASE ("a device that starts with a shape the engine was not built for is reported", "[engine][device][binding]")
{
    ScopedLogCapture capture;
    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = 256 } };
    DeviceBinding binding { engine };
    FakeAudioIODevice device { FakeDeviceShape { .numOutputChannels = 6, .bufferSize = 512 } };

    binding.audioDeviceAboutToStart (&device);

    const auto mismatch = eventsNamed (capture, "engine.device_engine_mismatch");
    REQUIRE (mismatch.size() == 1);
    CHECK (mismatch[0]["lvl"] == "warn");
    CHECK (mismatch[0]["output_channels"] == 6);
    CHECK (mismatch[0]["buffer_size"] == 512);
    CHECK (mismatch[0]["engine_output_channels"] == 2);
    CHECK (mismatch[0]["engine_max_block_size"] == 256);
}
