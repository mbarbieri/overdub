#include "ui/Intent.h"

namespace overdub::ui
{
namespace
{
char32_t lowerCase (char32_t character)
{
    if (character >= U'A' && character <= U'Z')
        return character - U'A' + U'a';
    return character;
}
} // namespace

const char* modeName (Mode mode)
{
    switch (mode)
    {
        case Mode::record: return "record";
        case Mode::edit: return "edit";
        case Mode::exportSong: return "export";
    }
    return "unknown";
}

const char* originName (Origin origin)
{
    switch (origin)
    {
        case Origin::user: return "user";
        case Origin::control: return "control";
    }
    return "unknown";
}

std::string_view opName (const Intent& intent)
{
    return core::operationName (intent);
}

std::optional<Intent> intentForKey (KeyChord key)
{
    const auto character = lowerCase (key.character);

    if (key.command)
    {
        if (character == U'z')
            return key.shift ? Intent { SongIntent { intent::Redo {} } } : Intent { SongIntent { intent::Undo {} } };
        return std::nullopt;
    }
    if (key.shift)
        return std::nullopt;

    switch (character)
    {
        case U'1': return WindowIntent { intent::ShowMode { Mode::record } };
        case U'2': return WindowIntent { intent::ShowMode { Mode::edit } };
        case U'3': return WindowIntent { intent::ShowMode { Mode::exportSong } };
        case U'r': return SongIntent { intent::Record {} };
        case U' ': return SongIntent { intent::PlayOrStop {} };
        case U's': return SongIntent { intent::SplitSelectedClip {} };
        case backspaceCharacter:
        case forwardDeleteCharacter: return SongIntent { intent::DeleteSelection {} };
        case leftArrowCharacter: return SongIntent { intent::Nudge { Direction::back } };
        case rightArrowCharacter: return SongIntent { intent::Nudge { Direction::forward } };
        default: return std::nullopt;
    }
}
} // namespace overdub::ui
