#pragma once

#include "core/Error.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <optional>
#include <string>
#include <vector>

// The audio device choice for this machine (issue #24). It is a per-machine setting, never part
// of the Song: a Song moved to another Mac plays through that Mac's device. Stored as JSON in
// ~/Library/Application Support/Overdub/audio-device.json.
namespace overdub::engine
{
// Empty strings and zeros mean "the system default".
struct DeviceSettings
{
    std::string deviceType;   // JUCE device type name, e.g. "CoreAudio"
    std::string inputDevice;  // device name as JUCE lists it
    std::string outputDevice; // device name as JUCE lists it
    int sampleRate = 0;       // Hz
    int bufferSize = 0;

    bool operator== (const DeviceSettings&) const = default;
};

// ~/Library/Application Support/Overdub/audio-device.json
juce::File defaultDeviceSettingsFile();

// The settings as a flat JSON object: the settings file's keys (without `version`), and the
// fields any log event uses to describe a device choice.
core::Fields deviceSettingsFields (const DeviceSettings& settings);

// Returns nullopt when the file does not exist (first launch on this machine).
// Errors: engine.device_settings_unreadable (path, reason).
core::Expected<std::optional<DeviceSettings>> readDeviceSettings (const juce::File& file);

// Creates the parent folder if needed and replaces the file atomically.
// Errors: engine.device_settings_write_failed (path).
core::Expected<void> writeDeviceSettings (const juce::File& file, const DeviceSettings& settings);

// The setup to pass to AudioDeviceManager::setAudioDeviceSetup (after selecting deviceType).
// Uses each device's default channels.
juce::AudioDeviceManager::AudioDeviceSetup toAudioDeviceSetup (const DeviceSettings& settings);

// The settings that reopen the device described by this type name and setup.
DeviceSettings deviceSettingsFrom (const juce::String& deviceType, const juce::AudioDeviceManager::AudioDeviceSetup& setup);

//==============================================================================
// Device changes (issue #41, ADR 0004 amendment). Overdub follows this machine's saved choice, not
// the system default: a default that changes is ignored while the saved choice runs.

// The device names of the current device type, as JUCE lists them now.
struct AvailableDevices
{
    std::vector<std::string> inputs;
    std::vector<std::string> outputs;
};

// What a device list or device state change means for the live engine.
enum class DeviceChange
{
    none,                 // the engine's device runs as it was opened
    lost,                 // the engine's device closed or was replaced by another (unplugged)
    reconfigured,         // the same device now runs at another rate or buffer size
    savedChoiceAvailable, // running on a fallback, and the saved choice is plugged in again
    appeared              // no device was open, and an output device is plugged in now
};

// "none", "lost", "reconfigured", "saved_choice_available", "appeared": the `change` field of
// app.device_changed.
const char* deviceChangeName (DeviceChange change);

// engineDevice: the device the live engine was built for (none without one); runningDevice: what
// the device manager runs now; savedChoice: this machine's choice (none when never saved);
// outputsWhenOpened: the output devices listed when the engine was built. Without a device, only
// an output device not listed then has `appeared` (one listed then already failed to open).
DeviceChange classifyDeviceChange (const std::optional<DeviceSettings>& engineDevice,
                                   const std::optional<DeviceSettings>& runningDevice,
                                   const std::optional<DeviceSettings>& savedChoice,
                                   const AvailableDevices& available,
                                   const std::vector<std::string>& outputsWhenOpened = {});

// The rate to reopen the device at for an open Song of songRate Hz: songRate when the device runs
// at another rate and offers it; none otherwise (already there, not offered, or no Song rate).
std::optional<int> sampleRateForSong (int deviceRate, int songRate, const std::vector<int>& offeredRates);
} // namespace overdub::engine
