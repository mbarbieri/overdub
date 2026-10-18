#pragma once

#include "capture/PassRecorder.h"
#include "core/RtEventRing.h"
#include "engine/DeviceBinding.h"
#include "engine/DeviceSettings.h"
#include "engine/Engine.h"
#include "engine/LatencyCalibration.h"
#include "ui/DeviceSettingsPanel.h"
#include "ui/LiveAudio.h"
#include "ui/OpenSong.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_events/juce_events.h>

#include <functional>
#include <memory>
#include <optional>

// The app's live audio: opens the audio device chosen on this machine (engine/DeviceSettings.h),
// builds the Engine for that device's shape, and binds the two (engine/DeviceBinding.h). It also
// runs what the engine itself does not: the RT event drain thread and the housekeeping timer.
// Message thread only. It is the app's ui::LiveAudio: a new device choice is applied by saving it
// and building a new LiveEngine.
//
// Device changes (issue #41, ADR 0004 amendment): it listens to the device manager and reports a
// change that matters (engine/DeviceSettings.h, classifyDeviceChange) through onDeviceChange,
// logged as app.device_changed. The owner answers by building a new LiveEngine, never from inside
// the callback. A change is reported once: the same classification is not reported again until
// another one was seen, so a saved choice that is plugged in but will not open causes no loop.
namespace overdub::app
{
class LiveEngine final : public ui::LiveAudio, private juce::Timer, private juce::ChangeListener
{
public:
    // Opens the saved device, or the system default when there is none or it fails to open.
    // Writes the opened device as this machine's choice when no choice was saved yet. With
    // songSampleRate (the open Song's rate), a device that runs at another rate but offers that one
    // is switched to it (app.device_rate_matched); the saved choice keeps its own rate.
    explicit LiveEngine (std::optional<int> songSampleRate = std::nullopt);
    // Stops the timer, unregisters the binding and closes the device; the members then go in
    // reverse declaration order (see below).
    ~LiveEngine() override;

    LiveEngine (const LiveEngine&) = delete;
    LiveEngine& operator= (const LiveEngine&) = delete;

    // Null when no output device could be opened.
    engine::Engine* engine() override { return audioEngine.get(); }

    // The open device's reported input and output latency and this machine's calibration offset
    // for it (ADR 0002): what a Pass recorded now is placed with. All 0 without a device.
    capture::RecordLatency recordLatency() const override { return latency; }

    // The open device's key in this machine's latency calibration file (ADR 0002); empty names and
    // rate 0 without a device.
    const engine::CalibrationKey& calibrationKey() const override { return key; }
    // The offset stored for calibrationKey(); none when the device was never calibrated at this rate
    // (or the file is unreadable): Takes are then placed with reported latency only.
    std::optional<song::SampleCount> storedCalibrationOffset() const override { return storedCalibration; }
    // A calibration just stored for this device (issue #39): Passes recorded from now are placed
    // with it, and the device binding logs it if the device starts again. The device and the engine
    // keep running.
    void setCalibrationOffset (song::SampleCount offsetSamples) override;

    // The open device's sample rate in Hz; 0 without a device.
    int sampleRate() const override { return deviceSampleRate; }

    // The open device as report fields: device_type, input_device, output_device, sample_rate,
    // buffer_size. Empty without a device.
    core::Fields deviceFields();

    // What a OpenSong plays and records through: the engine (null without a device), the
    // record latency and the active input channel names, in process() order.
    ui::AudioBinding audioBinding() override;

    // The settings window's offer: this device type's input and output devices, the open
    // device's sample rates and buffer sizes, and the current choice.
    ui::DeviceChoices deviceChoices() override;
    ui::DeviceOffer offerFor (const std::string& inputDevice, const std::string& outputDevice) override;

private:
    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster* source) override;
    engine::AvailableDevices listDevices();
    engine::DeviceChange classifyCurrentDevice();
    void matchSongRate (int songSampleRate);

    void openDevice();
    void startEngine (juce::AudioIODevice& device);

    // Invariant: members are destroyed in reverse declaration order, so each one only refers to
    // members declared above it. deviceManager (declared last, destroyed first) calls binding;
    // drainThread reads the engine's ring; binding calls the engine; audioEngine goes last.
    capture::RecordLatency latency;
    engine::CalibrationKey key;
    std::optional<song::SampleCount> storedCalibration;
    int deviceSampleRate = 0;
    std::optional<engine::DeviceSettings> savedChoice; // as read when opening
    std::optional<engine::DeviceSettings> engineDevice; // what audioEngine was built for
    std::vector<std::string> outputsWhenOpened; // the output devices listed when the engine was built
    engine::DeviceChange lastSeenChange = engine::DeviceChange::none;
    std::unique_ptr<engine::Engine> audioEngine;
    std::unique_ptr<engine::DeviceBinding> binding;
    std::optional<core::RtEventDrainThread> drainThread;
    juce::AudioDeviceManager deviceManager;
};
} // namespace overdub::app
