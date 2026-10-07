#pragma once

#include <optional>

// The main window's keyboard commands (issue #9 resolution, issue #27): keys 1-3 pick a mode,
// R records a Pass in any mode, Space plays or stops, command-Z / shift-command-Z undo and redo.
// Pure: no JUCE, so the mapping is unit-tested on its own.
namespace overdub::ui
{
enum class Mode
{
    record,
    edit,
    exportSong
};

enum class Command
{
    showRecord,
    showEdit,
    showExport,
    record,
    playOrStop,
    stop,
    rewind,
    undo,
    redo
};

// A key press as the window sees it: the character typed (letters in either case) and the
// modifiers held. `command` is the macOS command key.
struct KeyChord
{
    char32_t character = 0;
    bool command = false;
    bool shift = false;
};

// The command a key press triggers, or nullopt when it triggers none.
std::optional<Command> commandForKey (KeyChord key);

// The mode a show command picks; nullopt for every other command.
std::optional<Mode> modeFor (Command command);

// "record", "edit" or "export": the name in ui.* event fields.
const char* modeName (Mode mode);
} // namespace overdub::ui
