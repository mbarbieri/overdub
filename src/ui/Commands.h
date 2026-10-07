#pragma once

#include <optional>

// The main window's keyboard commands (issue #9 resolution, issue #27): keys 1-3 pick a mode,
// R records a Pass in any mode, Space plays or stops, command-Z / shift-command-Z undo and redo.
// In Edit mode (issue #28) S splits the selected Clip at the playhead and backspace (or forward
// delete) deletes it; the window ignores both in the other modes.
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
    rewind,
    undo,
    redo,
    splitClip,
    deleteClip
};

// The characters macOS delivers for the backspace key (labelled delete) and the forward delete
// key, as JUCE's KeyPress::backspaceKey and KeyPress::deleteKey report them.
inline constexpr char32_t backspaceCharacter = 0x7f;
inline constexpr char32_t forwardDeleteCharacter = 0xf728;

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
