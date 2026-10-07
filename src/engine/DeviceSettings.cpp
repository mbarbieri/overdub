#include "engine/DeviceSettings.h"

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
    const nlohmann::json document {
        { "version", settingsVersion },
        { "device_type", settings.deviceType },
        { "input_device", settings.inputDevice },
        { "output_device", settings.outputDevice },
        { "sample_rate", settings.sampleRate },
        { "buffer_size", settings.bufferSize },
    };

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
} // namespace overdub::engine
