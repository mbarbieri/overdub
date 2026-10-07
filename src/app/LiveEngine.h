#pragma once

#include "core/RtEventRing.h"
#include "engine/DeviceBinding.h"
#include "engine/Engine.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_events/juce_events.h>

#include <memory>
#include <optional>

// The app's live audio: opens the audio device chosen on this machine (engine/DeviceSettings.h),
// builds the Engine for that device's shape, and binds the two (engine/DeviceBinding.h). It also
// runs what the engine itself does not: the RT event drain thread and the housekeeping timer.
// Message thread only. Device-change handling and device choice UI are slice 20.
namespace overdub::app
{
class LiveEngine final : private juce::Timer
{
public:
    // Opens the saved device, or the system default when there is none or it fails to open.
    // Writes the opened device as this machine's choice when no choice was saved yet.
    LiveEngine();
    // Stops the timer, unregisters the binding and closes the device; the members then go in
    // reverse declaration order (see below).
    ~LiveEngine() override;

    LiveEngine (const LiveEngine&) = delete;
    LiveEngine& operator= (const LiveEngine&) = delete;

    // Null when no output device could be opened.
    engine::Engine* engine() { return audioEngine.get(); }

private:
    void timerCallback() override;

    void openDevice();
    void startEngine (juce::AudioIODevice& device);

    // Invariant: members are destroyed in reverse declaration order, so each one only refers to
    // members declared above it. deviceManager (declared last, destroyed first) calls binding;
    // drainThread reads the engine's ring; binding calls the engine; audioEngine goes last.
    std::unique_ptr<engine::Engine> audioEngine;
    std::unique_ptr<engine::DeviceBinding> binding;
    std::optional<core::RtEventDrainThread> drainThread;
    juce::AudioDeviceManager deviceManager;
};
} // namespace overdub::app
