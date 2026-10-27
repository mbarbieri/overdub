#include "ui/LastSong.h"

#include "core/Log.h"
#include "engine/DeviceSettings.h"
#include "ui/MainComponent.h"

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>

namespace overdub::ui
{
namespace
{
constexpr int lastSongVersion = 1;

std::string pathOf (const juce::File& file)
{
    return file.getFullPathName().toStdString();
}

core::Error unreadable (const juce::File& file, const std::string& reason)
{
    return core::Error { "ui.last_song_unreadable", "the Last Song file could not be read", { { "path", pathOf (file) }, { "reason", reason } } };
}
} // namespace

juce::File defaultLastSongFile()
{
    return engine::defaultDeviceSettingsFile().getSiblingFile ("last-song.json");
}

juce::File lastSongFileFor (const char* overrideVariable, bool scriptedLaunch)
{
    if (overrideVariable != nullptr && std::string_view (overrideVariable) != "")
        return juce::File::getCurrentWorkingDirectory().getChildFile (juce::String::fromUTF8 (overrideVariable));
    return scriptedLaunch ? juce::File() : defaultLastSongFile();
}

core::Expected<std::optional<juce::File>> readLastSong (const juce::File& file)
{
    if (! file.existsAsFile())
        return std::optional<juce::File> {};

    const auto document = nlohmann::json::parse (file.loadFileAsString().toStdString(), nullptr, false);
    if (document.is_discarded() || ! document.is_object())
        return std::unexpected (unreadable (file, "not a JSON object"));

    try
    {
        if (document.at ("version").get<int>() != lastSongVersion)
            return std::unexpected (unreadable (file, "unsupported version"));
        const auto path = juce::String::fromUTF8 (document.at ("path").get<std::string>().c_str());
        if (! juce::File::isAbsolutePath (path))
            return std::unexpected (unreadable (file, "path is not absolute"));
        return std::optional<juce::File> { juce::File (path) };
    }
    catch (const nlohmann::json::exception& error)
    {
        return std::unexpected (unreadable (file, error.what()));
    }
}

core::Expected<void> writeLastSong (const juce::File& file, const juce::File& package)
{
    const nlohmann::json document { { "version", lastSongVersion }, { "path", pathOf (package) } };
    const auto text = document.dump (1, ' ', false, nlohmann::json::error_handler_t::replace);
    // replaceWithText writes a temporary file and renames it over the target.
    const auto written = file.getParentDirectory().createDirectory().wasOk()
                      && file.replaceWithText (juce::String::fromUTF8 (text.data(), (int) text.size()));
    if (! written)
        return std::unexpected (core::Error { "ui.last_song_write_failed", "the Last Song file could not be written", { { "path", pathOf (file) } } });
    return {};
}

core::Expected<void> forgetLastSong (const juce::File& file)
{
    if (! file.deleteFile())
        return std::unexpected (core::Error { "ui.last_song_remove_failed", "the Last Song file could not be removed", { { "path", pathOf (file) } } });
    return {};
}
void reopenLastSongAtLaunch (MainComponent& content, bool skipLastSong)
{
    if (skipLastSong)
    {
        core::logEvent (core::Level::info, "ui.last_song_skipped", { { "reason", "shift_held" } });
        return;
    }
    content.run (AppIntent { intent::ReopenLastSong {} });
}
} // namespace overdub::ui
