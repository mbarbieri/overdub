#include "engine/DeviceSettings.h"

#include <algorithm>

#include <nlohmann/json.hpp>

namespace overdub::engine
{
namespace
{
constexpr int settingsVersion = 1;

std::string pathOf (const juce::File& file)
{
    return file.getFullPathName().toStdString();
}

core::Error unreadable (const juce::File& file, const std::string& reason)
{
    return core::Error { "engine.device_settings_unreadable",
                         "the audio device settings file could not be read",
                         { { "path", pathOf (file) }, { "reason", reason } } };
}
} // namespace

juce::File defaultDeviceSettingsFile()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
        .getChildFile ("Application Support")
        .getChildFile ("Overdub")
        .getChildFile ("audio-device.json");
}

core::Fields deviceSettingsFields (const DeviceSettings& settings)
{
    return {
        { "device_type", settings.deviceType },
        { "input_device", settings.inputDevice },
        { "output_device", settings.outputDevice },
        { "sample_rate", settings.sampleRate },
        { "buffer_size", settings.bufferSize },
    };
}

core::Expected<std::optional<DeviceSettings>> readDeviceSettings (const juce::File& file)
{
    if (! file.existsAsFile())
        return std::optional<DeviceSettings> {};

    const auto text = file.loadFileAsString().toStdString();
    const auto document = nlohmann::json::parse (text, nullptr, false);
    if (document.is_discarded() || ! document.is_object())
        return std::unexpected (unreadable (file, "not a JSON object"));

    try
    {
        if (document.at ("version").get<int>() != settingsVersion)
            return std::unexpected (unreadable (file, "unsupported version"));

        return std::optional<DeviceSettings> { DeviceSettings {
            .deviceType = document.at ("device_type").get<std::string>(),
            .inputDevice = document.at ("input_device").get<std::string>(),
            .outputDevice = document.at ("output_device").get<std::string>(),
            .sampleRate = document.at ("sample_rate").get<int>(),
            .bufferSize = document.at ("buffer_size").get<int>(),
        } };
    }
    catch (const nlohmann::json::exception& error)
    {
        return std::unexpected (unreadable (file, error.what()));
    }
}

core::Expected<void> writeDeviceSettings (const juce::File& file, const DeviceSettings& settings)
{
    auto document = deviceSettingsFields (settings);
    document["version"] = settingsVersion;

    // Device names come from the OS; replace invalid UTF-8 rather than throw.
    const auto text = document.dump (1, ' ', false, nlohmann::json::error_handler_t::replace);
    // replaceWithText writes a temporary file and renames it over the target.
    const auto written = file.getParentDirectory().createDirectory().wasOk()
                      && file.replaceWithText (juce::String::fromUTF8 (text.data(), (int) text.size()));
    if (! written)
        return std::unexpected (core::Error { "engine.device_settings_write_failed",
                                              "the audio device settings file could not be written",
                                              { { "path", pathOf (file) } } });

    core::logEvent (core::Level::info, "engine.device_settings_saved", { { "path", pathOf (file) } });
    return {};
}

juce::AudioDeviceManager::AudioDeviceSetup toAudioDeviceSetup (const DeviceSettings& settings)
{
    juce::AudioDeviceManager::AudioDeviceSetup setup;
    setup.inputDeviceName = juce::String::fromUTF8 (settings.inputDevice.c_str());
    setup.outputDeviceName = juce::String::fromUTF8 (settings.outputDevice.c_str());
    setup.sampleRate = settings.sampleRate;
    setup.bufferSize = settings.bufferSize;
    setup.useDefaultInputChannels = true;
    setup.useDefaultOutputChannels = true;
    return setup;
}

DeviceSettings deviceSettingsFrom (const juce::String& deviceType, const juce::AudioDeviceManager::AudioDeviceSetup& setup)
{
    return DeviceSettings {
        .deviceType = deviceType.toStdString(),
        .inputDevice = setup.inputDeviceName.toStdString(),
        .outputDevice = setup.outputDeviceName.toStdString(),
        .sampleRate = juce::roundToInt (setup.sampleRate),
        .bufferSize = setup.bufferSize,
    };
}
namespace
{
bool sameDevices (const DeviceSettings& a, const DeviceSettings& b)
{
    return a.deviceType == b.deviceType && a.inputDevice == b.inputDevice && a.outputDevice == b.outputDevice;
}

// An empty input name means "no input device chosen": only the output must match.
bool runsSavedChoice (const DeviceSettings& running, const DeviceSettings& saved)
{
    return running.outputDevice == saved.outputDevice && (saved.inputDevice.empty() || running.inputDevice == saved.inputDevice);
}

bool isAvailable (const DeviceSettings& saved, const AvailableDevices& available)
{
    if (saved.outputDevice.empty() || ! std::ranges::contains (available.outputs, saved.outputDevice))
        return false;
    return saved.inputDevice.empty() || std::ranges::contains (available.inputs, saved.inputDevice);
}
} // namespace

const char* deviceChangeName (DeviceChange change)
{
    switch (change)
    {
        case DeviceChange::none: return "none";
        case DeviceChange::lost: return "lost";
        case DeviceChange::reconfigured: return "reconfigured";
        case DeviceChange::savedChoiceAvailable: return "saved_choice_available";
        case DeviceChange::appeared: return "appeared";
    }
    return "unknown";
}

DeviceChange classifyDeviceChange (const std::optional<DeviceSettings>& engineDevice,
                                   const std::optional<DeviceSettings>& runningDevice,
                                   const std::optional<DeviceSettings>& savedChoice,
                                   const AvailableDevices& available,
                                   const std::vector<std::string>& outputsWhenOpened)
{
    if (! engineDevice.has_value())
    {
        const auto isNew = [&] (const std::string& output) { return ! std::ranges::contains (outputsWhenOpened, output); };
        return std::ranges::any_of (available.outputs, isNew) ? DeviceChange::appeared : DeviceChange::none;
    }
    if (! runningDevice.has_value() || ! sameDevices (*engineDevice, *runningDevice))
        return DeviceChange::lost;
    if (runningDevice->sampleRate != engineDevice->sampleRate || runningDevice->bufferSize != engineDevice->bufferSize)
        return DeviceChange::reconfigured;
    if (savedChoice.has_value() && ! runsSavedChoice (*engineDevice, *savedChoice) && isAvailable (*savedChoice, available))
        return DeviceChange::savedChoiceAvailable;
    return DeviceChange::none;
}

std::optional<int> sampleRateForSong (int deviceRate, int songRate, const std::vector<int>& offeredRates)
{
    if (songRate <= 0 || songRate == deviceRate || ! std::ranges::contains (offeredRates, songRate))
        return std::nullopt;
    return songRate;
}
} // namespace overdub::engine
