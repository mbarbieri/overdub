#pragma once

#include "song/Song.h"
#include "ui/Mode.h"

#include <optional>

// The Selection (GLOSSARY.md, issue #259): the one Clip, Video Take, Marker or Loop the user has
// picked to act on, held by ui::SongView while the Song is open and never saved with it. These are
// its rules, as pure functions of what is held, the Song and the window's mode:
//
// - Picking one clears whatever was selected. Record mode never selects a Marker or a Loop
//   (picking one there moves the record point), and Transcribe mode never selects a Loop (picking
//   one activates it, which is the Song's setting, not the Selection). A Clip or a Video Take is
//   selected in every mode.
// - A click on nothing (a pick of none) clears the Selection; an empty Loop/Marker strip clears only
//   a selected Marker or Loop.
// - The Selection is held by id and read against the Song (resolvedSelection): an id the Song no
//   longer has reads as none, and reads again once an undo or redo brings it back. It survives a
//   change of mode.
// - Backspace deletes nothing in Record mode, the Selection in Edit mode, and in Transcribe mode the
//   selected Marker or else the active Loop (deleteTarget).
namespace overdub::ui
{
enum class SelectionKind
{
    clip,
    videoTake,
    marker,
    loop
};

struct Selection
{
    SelectionKind kind = SelectionKind::clip;
    song::Id id;

    bool operator== (const Selection&) const = default;
};

// The id of `selection` when it is of this kind; none for another kind or none. It does not read
// the Song: use selectedId for what is held.
std::optional<song::Id> idOf (const std::optional<Selection>& selection, SelectionKind kind);

// What is held, while the Song has it; none otherwise.
std::optional<Selection> resolvedSelection (const std::optional<Selection>& held, const song::Song& song);

// The id of that kind the resolved Selection is; none when it is another kind or none.
std::optional<song::Id> selectedId (const std::optional<Selection>& held, SelectionKind kind, const song::Song& song);

// The Selection after picking `picked` of this kind in this mode (none: a click on nothing).
std::optional<Selection> selectionAfterPick (const std::optional<Selection>& held, SelectionKind kind, const std::optional<song::Id>& picked, Mode mode);

// What Backspace deletes in this mode; none when it deletes nothing.
std::optional<Selection> deleteTarget (const std::optional<Selection>& held, const song::Song& song, Mode mode);
} // namespace overdub::ui
