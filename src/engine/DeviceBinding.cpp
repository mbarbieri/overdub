#include "engine/DeviceBinding.h"

#include "core/Log.h"

namespace overdub::engine
{
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

void DeviceBinding::audioDeviceAboutToStart (juce::AudioIODevice*)
{
}

void DeviceBinding::audioDeviceStopped()
{
}
} // namespace overdub::engine
