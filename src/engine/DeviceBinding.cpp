#include "engine/DeviceBinding.h"

#include "core/Log.h"

namespace overdub::engine
{
namespace
{
    // ADR 0002: the loopback calibration offset defaults to 0 until the user calibrates; the
    // calibration and its per-machine storage arrive with that slice.
    constexpr int calibrationOffsetSamples = 0;

    void reportShapeMismatch (juce::AudioIODevice& device, const EngineConfig& config)
    {
        const auto deviceConfig = engineConfigFor (device);
        const auto fits = deviceConfig.numOutputChannels == config.numOutputChannels
                       && deviceConfig.maxBlockSize <= config.maxBlockSize;
        if (fits)
            return;

        core::logEvent (core::Level::warn,
                        "engine.device_engine_mismatch",
                        {
                            { "device", device.getName().toStdString() },
                            { "output_channels", deviceConfig.numOutputChannels },
                            { "buffer_size", deviceConfig.maxBlockSize },
                            { "engine_output_channels", config.numOutputChannels },
                            { "engine_max_block_size", config.maxBlockSize },
                        });
    }
} // namespace

EngineConfig engineConfigFor (juce::AudioIODevice& device)
{
    return EngineConfig {
        .numOutputChannels = device.getActiveOutputChannels().countNumberOfSetBits(),
        .maxBlockSize = device.getCurrentBufferSizeSamples(),
    };
}

DeviceBinding::DeviceBinding (Engine& engineToDrive)
    : engine (engineToDrive)
{
}

void DeviceBinding::audioDeviceIOCallbackWithContext (const float* const* inputs,
                                                      int numInputChannels,
                                                      float* const* outputs,
                                                      int numOutputChannels,
                                                      int numSamples,
                                                      const juce::AudioIODeviceCallbackContext& context)
{
    // A plain thread_local store: cheap enough for every callback, and it follows the device
    // onto a new thread when it restarts. The logger drops JUCE messages on `audio` threads.
    core::setCurrentThreadRole (core::ThreadRole::audio);
    const auto hostTimeNs = context.hostTimeNs != nullptr ? *context.hostTimeNs : core::hostTimeNowNs();
    engine.process (inputs, numInputChannels, outputs, numOutputChannels, numSamples, hostTimeNs);
}

void DeviceBinding::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    runningDevice = device;
    if (device == nullptr)
        return;

    core::logEvent (core::Level::info,
                    "engine.device_opened",
                    {
                        { "device", device->getName().toStdString() },
                        { "type", device->getTypeName().toStdString() },
                        { "sample_rate", juce::roundToInt (device->getCurrentSampleRate()) },
                        { "buffer_size", device->getCurrentBufferSizeSamples() },
                        { "input_channels", device->getActiveInputChannels().countNumberOfSetBits() },
                        { "output_channels", device->getActiveOutputChannels().countNumberOfSetBits() },
                        { "input_latency_samples", device->getInputLatencyInSamples() },
                        { "output_latency_samples", device->getOutputLatencyInSamples() },
                        { "calibration_offset_samples", calibrationOffsetSamples },
                    });
    reportShapeMismatch (*device, engine.configuration());
}

void DeviceBinding::audioDeviceStopped()
{
    if (runningDevice == nullptr)
        return;

    core::logEvent (core::Level::info,
                    "engine.device_closed",
                    {
                        { "device", runningDevice->getName().toStdString() },
                        { "type", runningDevice->getTypeName().toStdString() },
                        { "xruns", runningDevice->getXRunCount() },
                    });
    runningDevice = nullptr;
}
} // namespace overdub::engine
