#include "ui/WindowSettings.h"

#include "ui/LastSong.h"

#include <catch2/catch_test_macros.hpp>

using namespace overdub::ui;

// The window settings (issue #225): what the main window remembers per Mac, beside the Last Song.
namespace
{
struct TempFolder
{
    juce::File folder = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("overdub-window-settings-" + juce::Uuid().toString());
    TempFolder() { REQUIRE (folder.createDirectory().wasOk()); }
    ~TempFolder() { folder.deleteRecursively(); }
};
} // namespace

TEST_CASE ("a missing window settings file reads as the defaults, and a written one reads back", "[ui][window_settings]")
{
    TempFolder temp;
    const auto file = temp.folder.getChildFile ("nested").getChildFile ("window-settings.json");

    auto missing = readWindowSettings (file);
    REQUIRE (missing.has_value());
    CHECK (*missing == WindowSettings {});
    CHECK (missing->transcribeColumnVisible);

    REQUIRE (writeWindowSettings (file, WindowSettings { .transcribeColumnVisible = false }).has_value());
    auto read = readWindowSettings (file);
    REQUIRE (read.has_value());
    CHECK_FALSE (read->transcribeColumnVisible);
}

TEST_CASE ("an unreadable window settings file is ui.window_settings_unreadable with its path and reason", "[ui][window_settings]")
{
    TempFolder temp;
    const auto file = temp.folder.getChildFile ("window-settings.json");
    for (const auto* text : { "not json", "[1]", R"({"version":2,"transcribe_column_visible":false})", R"({"version":1,"transcribe_column_visible":"no"})" })
    {
        REQUIRE (file.replaceWithText (text));
        const auto read = readWindowSettings (file);
        REQUIRE_FALSE (read.has_value());
        CHECK (read.error().code == "ui.window_settings_unreadable");
        CHECK (read.error().fields.at ("path") == file.getFullPathName().toStdString());
        CHECK (read.error().fields.contains ("reason"));
    }
}

TEST_CASE ("the window settings live beside the Last Song, and a scripted launch neither reads nor writes them", "[ui][window_settings]")
{
    const auto defaultFile = defaultWindowSettingsFile();
    CHECK (defaultFile.getFileName() == "window-settings.json");
    CHECK (defaultFile.getParentDirectory() == defaultLastSongFile().getParentDirectory());

    CHECK (windowSettingsFileFor (nullptr, false) == defaultFile);
    CHECK (windowSettingsFileFor ("", false) == defaultFile);
    CHECK (windowSettingsFileFor (nullptr, true) == juce::File());
    CHECK (windowSettingsFileFor ("/tmp/run/window-settings.json", true) == juce::File ("/tmp/run/window-settings.json"));
}
