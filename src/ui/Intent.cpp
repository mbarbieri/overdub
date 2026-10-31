#include "ui/Intent.h"

#include "song/PracticeSpeed.h"

#include <algorithm>
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

// H: half speed, or back to normal from half speed.
int halfOrNormalPracticeSpeed (int speedPercent)
{
    return speedPercent == song::halfPracticeSpeed ? song::normalPracticeSpeed : song::halfPracticeSpeed;
}

// - and =: one step down or up, clamped to the range.
int steppedPracticeSpeed (int speedPercent, int steps)
{
    return std::clamp (speedPercent + steps * song::practiceSpeedStep, song::minPracticeSpeed, song::maxPracticeSpeed);
}

template <typename Leaf>
concept LeafRunsPerFrame = Leaf::runsPerFrame;
} // namespace

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

std::optional<Intent> intentForKey (KeyChord key, KeyContext context)
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
    if (key.option)
    {
        if (key.shift)
            return std::nullopt;
        if (character == leftArrowCharacter || character == rightArrowCharacter)
            return SongIntent { intent::SelectAdjacentLoop { character == leftArrowCharacter ? Direction::back : Direction::forward } };
        if (character >= U'1' && character <= U'9')
        {
            const auto index = (std::size_t) (character - U'1');
            if (index >= context.loopIds.size())
                return std::nullopt;
            return SongIntent { intent::SelectLoop { context.loopIds[index] } };
        }
        return std::nullopt;
    }
    if (key.shift)
        return character == U'r' ? std::optional<Intent> (SongIntent { intent::RecordAgain {} }) : std::nullopt;

    switch (character)
    {
        case U'1': return WindowIntent { intent::ShowMode { Mode::record } };
        case U'2': return WindowIntent { intent::ShowMode { Mode::edit } };
        case U'3': return WindowIntent { intent::ShowMode { Mode::transcribe } };
        case U'i': return WindowIntent { intent::SetTranscribeColumnVisible { ! context.transcribeColumnVisible } };
        case U'k': return SongIntent { intent::AddMarker {} };
        case U'l': return SongIntent { intent::SetLooping { ! context.looping } };
        case U'[': return SongIntent { intent::MarkLoopStart {} };
        case U']': return SongIntent { intent::MarkLoopEnd {} };
        case U'h': return SongIntent { intent::SetPracticeSpeed { halfOrNormalPracticeSpeed (context.practiceSpeed) } };
        case U'-': return SongIntent { intent::SetPracticeSpeed { steppedPracticeSpeed (context.practiceSpeed, -1) } };
        case U'=': return SongIntent { intent::SetPracticeSpeed { steppedPracticeSpeed (context.practiceSpeed, 1) } };
        case escapeCharacter:
            if (context.loopStartPending)
                return SongIntent { intent::CancelLoopStart {} };
            return std::nullopt;
        case U'r': return SongIntent { intent::Record {} };
        case U' ': return SongIntent { intent::PlayOrStop {} };
        case U's': return SongIntent { intent::SplitSelectedClip {} };
        case backspaceCharacter:
        case forwardDeleteCharacter:
            if (const auto markerId = idOf (context.deleteTarget, SelectionKind::marker))
                return SongIntent { intent::Edit { intent::DeleteMarker { *markerId } } };
            if (const auto loopId = idOf (context.deleteTarget, SelectionKind::loop))
                return SongIntent { intent::Edit { intent::DeleteLoop { *loopId } } };
            return SongIntent { intent::DeleteSelection {} };
        case leftArrowCharacter: return SongIntent { intent::Nudge { Direction::back } };
        case rightArrowCharacter: return SongIntent { intent::Nudge { Direction::forward } };
        default: return std::nullopt;
    }
}
} // namespace overdub::ui
