#pragma once

#include "core/Error.h"

#include <juce_core/juce_core.h>

#include <optional>

// The Last Song (issue #206, GLOSSARY.md): the Song most recently opened successfully on this Mac,
// recorded in a per-machine JSON file beside the device settings ({"version":1,"path":...}). The
// Workspace records it once a Song has finished opening and reopens it for reopen_last_song,
// which launch runs unless Shift is held (issue #208).
namespace overdub::ui
{
class MainComponent;

// ~/Library/Application Support/Overdub/last-song.json
juce::File defaultLastSongFile();

// The file a launch reads and records the Last Song in: `overrideVariable`
// (OVERDUB_LAST_SONG_FILE) when set, else none for a scripted launch (one whose control socket was
// moved with OVERDUB_CONTROL_SOCKET), else the default. None: the Last Song is neither read nor
// recorded.
juce::File lastSongFileFor (const char* overrideVariable, bool scriptedLaunch);

// The recorded package; none when the file does not exist. ui.last_song_unreadable (path, reason).
core::Expected<std::optional<juce::File>> readLastSong (const juce::File& file);
// ui.last_song_write_failed (path).
core::Expected<void> writeLastSong (const juce::File& file, const juce::File& package);
// Removes the record: no Last Song. ui.last_song_remove_failed (path).
core::Expected<void> forgetLastSong (const juce::File& file);

// What the window runs once it opens: reopen_last_song, unless `skipLastSong` (Shift held at
// launch, issue #208), which runs nothing, keeps the Last Song recorded and logs
// ui.last_song_skipped. The caller reads the modifier.
void reopenLastSongAtLaunch (MainComponent& content, bool skipLastSong);
} // namespace overdub::ui
