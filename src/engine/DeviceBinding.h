#pragma once

#include "engine/Engine.h"

#include <juce_audio_devices/juce_audio_devices.h>

// Binds a live JUCE AudioIODevice to the engine (ADR 0005): the device's callback calls
// Engine::process with the device's buffers and output host time. Register it with
// AudioDeviceManager::addAudioCallback. The binding starts no threads; the owner runs the
// RtEventDrainThread and calls Engine::housekeeping.
namespace overdub::engine
{
// The engine shape for an open device: its active output channels and current buffer size.
// Build the Engine after opening the device, with this config.
EngineConfig engineConfigFor (juce::AudioIODevice& device);

class DeviceBinding final : public juce::AudioIODeviceCallback
{
public:
    // The engine must outlive the binding and stay registered no longer than it lives.
    // calibrationOffsetSamples: this machine's loopback calibration for the device and rate
    // (engine/LatencyCalibration.h, ADR 0002), logged with engine.device_opened.
    explicit DeviceBinding (Engine& engine, song::SampleCount calibrationOffsetSamples = 0);

    // Audio thread. Marks the calling thread as `audio` for logging, then calls Engine::process.
    // When the device reports no host time, the time the callback ran is used instead.
    void audioDeviceIOCallbackWithContext (const float* const* inputs,
                                           int numInputChannels,
                                           float* const* outputs,
                                           int numOutputChannels,
                                           int numSamples,
                                           const juce::AudioIODeviceCallbackContext& context) override;

    // Non-audio thread (the thread starting or stopping the device). Log engine.device_opened /
    // engine.device_closed (ADR 0006). A device whose shape differs from the engine's config is
    // logged as engine.device_engine_mismatch (warn); the engine then renders silence and
    // reports engine.block_rejected once per rejected shape. A stopped device (closed, unplugged,
    // or replaced by the device manager) calls Engine::deviceStopped: the transport stops and a
    // running Pass ends where the device stopped (issue #41).
    void audioDeviceAboutToStart (juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;
    // Logs engine.device_error (warn, `reason`); the device manager decides what follows.
    void audioDeviceError (const juce::String& errorMessage) override;

private:
    Engine& engine;
    song::SampleCount calibrationOffset;
    // Valid from audioDeviceAboutToStart until audioDeviceStopped; never read by the callback.
    juce::AudioIODevice* runningDevice = nullptr;
};
} // namespace overdub::engine
