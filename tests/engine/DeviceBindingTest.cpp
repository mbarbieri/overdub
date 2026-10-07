#include "engine/DeviceBinding.h"
#include "engine/Engine.h"

#include "EngineFixture.h"
#include "support/RealtimeGuardCatch.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using overdub::engine::DeviceBinding;
using overdub::engine::Engine;
using overdub::engine::EngineConfig;
using namespace overdub::engine::testing;

namespace
{
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
