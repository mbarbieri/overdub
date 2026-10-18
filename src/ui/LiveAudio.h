#pragma once

#include "capture/PassRecorder.h"
#include "engine/DeviceSettings.h"
#include "engine/Engine.h"
#include "engine/LatencyCalibration.h"
#include "ui/DeviceSettingsPanel.h"
#include "ui/OpenSong.h"

#include <functional>
#include <memory>
#include <optional>

// The live audio a Workspace plays and records through (issue #105): one opened device, the Engine
// built for it, and what this machine knows about it. app::LiveEngine is the app's adapter
// (CoreAudio through juce::AudioDeviceManager); tests use one over an Engine that a loopback fake
// device drives. A new device choice or a device change is answered by opening a new LiveAudio
// (OpenLiveAudio), never by changing this one.
//
// It is the one owner of the device's latency calibration offset: recordLatency() carries it, and
// setCalibrationOffset() changes it for every Pass recorded from then on.
//
// Message thread only.
namespace overdub::ui
{
class LiveAudio
{
public:
    virtual ~LiveAudio() = default;

    // Null when no output device could be opened.
    virtual engine::Engine* engine() = 0;
    // What an OpenSong plays and records through: the engine (null without a device), the record
    // latency, the active input channel names in process() order, the output device and the rate.
    virtual AudioBinding audioBinding() = 0;
    // The device's reported input and output latency and the calibration offset (ADR 0002). All 0
    // without a device.
    virtual capture::RecordLatency recordLatency() const = 0;
    // The device's key in this machine's latency calibration file; empty names and rate 0 without
    // a device.
    virtual const engine::CalibrationKey& calibrationKey() const = 0;
    // The offset stored for calibrationKey() when the device was opened, or set since; none when it
    // was never calibrated at this rate.
    virtual std::optional<song::SampleCount> storedCalibrationOffset() const = 0;
    // A calibration just stored for this device: Passes recorded from now are placed with it. The
    // device and the engine keep running.
    virtual void setCalibrationOffset (song::SampleCount offsetSamples) = 0;
    // The device's sample rate in Hz; 0 without a device.
    virtual int sampleRate() const = 0;
    // The settings window's offer for this device type, and the current choice.
    virtual DeviceChoices deviceChoices() = 0;
    // What a pair of devices of this device type offers, listed in the settings window before
    // they are applied (issue #159). Empty for a device that cannot be found.
    virtual DeviceOffer offerFor (const std::string& inputDevice, const std::string& outputDevice) = 0;

    // Called (message thread) with each device change that matters (engine::classifyDeviceChange).
    // The callee must not destroy this LiveAudio from inside the call.
    std::function<void (engine::DeviceChange)> onDeviceChange;
};

// Opens the live audio: the saved device choice, or the default, switched to songSampleRate when
// the device offers it (the Song's rate is fixed, ADR 0004).
using OpenLiveAudio = std::function<std::unique_ptr<LiveAudio> (std::optional<int> songSampleRate)>;
} // namespace overdub::ui
