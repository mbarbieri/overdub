#include "ui/Commands.h"

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

std::optional<Command> commandForKey (KeyChord key)
{
    const auto character = lowerCase (key.character);

    if (key.command)
    {
        if (character == U'z')
            return key.shift ? Command::redo : Command::undo;
        return std::nullopt;
    }
    if (key.shift)
        return character == U'r' ? std::optional { Command::record } : std::nullopt;

    switch (character)
    {
        case U'1': return Command::showRecord;
        case U'2': return Command::showEdit;
        case U'3': return Command::showExport;
        case U'r': return Command::record;
        case U' ': return Command::playOrStop;
        default: return std::nullopt;
    }
}

std::optional<Mode> modeFor (Command command)
{
    switch (command)
    {
        case Command::showRecord: return Mode::record;
        case Command::showEdit: return Mode::edit;
        case Command::showExport: return Mode::exportSong;
        case Command::record:
        case Command::playOrStop:
        case Command::stop:
        case Command::rewind:
        case Command::undo:
        case Command::redo:
            return std::nullopt;
    }
    return std::nullopt;
}

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
} // namespace overdub::ui
