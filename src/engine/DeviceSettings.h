#pragma once

#include "core/Error.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <optional>
#include <string>

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
} // namespace overdub::engine
