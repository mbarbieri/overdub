#pragma once

#include "core/Error.h"

#include <juce_core/juce_core.h>

// What the main window remembers per Mac, not in any Song (issue #225): whether Transcribe mode's
// right column shows. A per-machine JSON file beside the Last Song ({"version":1,
// "transcribe_column_visible":true}); a missing file means the defaults.
namespace overdub::ui
{
struct WindowSettings
{
    bool transcribeColumnVisible = true;

    bool operator== (const WindowSettings&) const = default;
};

// ~/Library/Application Support/Overdub/window-settings.json
juce::File defaultWindowSettingsFile();

// The file a launch reads and writes the window settings in: `overrideVariable`
// (OVERDUB_WINDOW_SETTINGS_FILE) when set, else none for a scripted launch (its control socket
// moved), else the default. None: the settings are neither read nor written.
juce::File windowSettingsFileFor (const char* overrideVariable, bool scriptedLaunch);

// The defaults when the file does not exist. ui.window_settings_unreadable (path, reason).
core::Expected<WindowSettings> readWindowSettings (const juce::File& file);
// ui.window_settings_write_failed (path).
core::Expected<void> writeWindowSettings (const juce::File& file, const WindowSettings& settings);
} // namespace overdub::ui
