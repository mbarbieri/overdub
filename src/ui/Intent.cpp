#include "ui/Intent.h"

#include <type_traits>
#include <variant>

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

template <typename Leaf>
concept LeafRunsPerFrame = Leaf::runsPerFrame;
} // namespace

const char* modeName (Mode mode)
{
    switch (mode)
    {
        case Mode::record: return "record";
        case Mode::edit: return "edit";
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

bool runsPerFrame (const Intent& intent)
{
    const auto leafRunsPerFrame = [] (const auto& leaf) { return LeafRunsPerFrame<std::decay_t<decltype (leaf)>>; };
    return std::visit ([&] (const auto& group) { return std::visit (leafRunsPerFrame, group); }, intent);
}

core::Error noSongOpen (std::string_view op)
{
    return core::Error { "ui.no_song_open", "no Song is open", { { "op", std::string (op) } } };
}

std::optional<Intent> intentForKey (KeyChord key)
{
    const auto character = lowerCase (key.character);

    if (key.command)
    {
        if (character == U'z')
            return key.shift ? Intent { SongIntent { intent::Redo {} } } : Intent { SongIntent { intent::Undo {} } };
        if (character == U'e' && ! key.shift)
            return SongIntent { intent::ShowExport { true } };
        return std::nullopt;
    }
    if (key.shift)
        return std::nullopt;

    switch (character)
    {
        case U'1': return WindowIntent { intent::ShowMode { Mode::record } };
        case U'2': return WindowIntent { intent::ShowMode { Mode::edit } };
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
