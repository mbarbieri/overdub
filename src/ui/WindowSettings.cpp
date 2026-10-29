#include "ui/WindowSettings.h"

#include "ui/LastSong.h"

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>

namespace overdub::ui
{
namespace
{
constexpr int windowSettingsVersion = 1;

std::string pathOf (const juce::File& file)
{
    return file.getFullPathName().toStdString();
}

core::Error unreadable (const juce::File& file, const std::string& reason)
{
    return core::Error { "ui.window_settings_unreadable", "the window settings file could not be read", { { "path", pathOf (file) }, { "reason", reason } } };
}
} // namespace

juce::File defaultWindowSettingsFile()
{
    return defaultLastSongFile().getSiblingFile ("window-settings.json");
}

juce::File windowSettingsFileFor (const char* overrideVariable, bool scriptedLaunch)
{
    if (overrideVariable != nullptr && std::string_view (overrideVariable) != "")
        return juce::File::getCurrentWorkingDirectory().getChildFile (juce::String::fromUTF8 (overrideVariable));
    return scriptedLaunch ? juce::File() : defaultWindowSettingsFile();
}

core::Expected<WindowSettings> readWindowSettings (const juce::File& file)
{
    if (! file.existsAsFile())
        return WindowSettings {};

    const auto document = nlohmann::json::parse (file.loadFileAsString().toStdString(), nullptr, false);
    if (document.is_discarded() || ! document.is_object())
        return std::unexpected (unreadable (file, "not a JSON object"));

    try
    {
        if (document.at ("version").get<int>() != windowSettingsVersion)
            return std::unexpected (unreadable (file, "unsupported version"));
        WindowSettings settings;
        settings.transcribeColumnVisible = document.value ("transcribe_column_visible", settings.transcribeColumnVisible);
        return settings;
    }
    catch (const nlohmann::json::exception& error)
    {
        return std::unexpected (unreadable (file, error.what()));
    }
}

core::Expected<void> writeWindowSettings (const juce::File& file, const WindowSettings& settings)
{
    const nlohmann::json document { { "version", windowSettingsVersion }, { "transcribe_column_visible", settings.transcribeColumnVisible } };
    const auto text = document.dump (1, ' ', false, nlohmann::json::error_handler_t::replace);
    // replaceWithText writes a temporary file and renames it over the target.
    const auto written = file.getParentDirectory().createDirectory().wasOk()
                      && file.replaceWithText (juce::String::fromUTF8 (text.data(), (int) text.size()));
    if (! written)
        return std::unexpected (core::Error { "ui.window_settings_write_failed", "the window settings file could not be written", { { "path", pathOf (file) } } });
    return {};
}
} // namespace overdub::ui
