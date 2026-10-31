#include "ui/Selection.h"

#include "song/ClipEdits.h"
#include "song/LoopEdits.h"
#include "song/MarkerEdits.h"
#include "song/VideoTakeEdits.h"

namespace overdub::ui
{
namespace
{
bool songHas (const song::Song& song, const Selection& selection)
{
    switch (selection.kind)
    {
        case SelectionKind::clip: return song::findClip (song, selection.id) != nullptr;
        case SelectionKind::videoTake: return song::findVideoTake (song, selection.id) != nullptr;
        case SelectionKind::marker: return song::findMarker (song, selection.id) != nullptr;
        case SelectionKind::loop: return song::findLoop (song, selection.id) != nullptr;
    }
    return false;
}

// Record moves the record point to a Marker or Loop it is given, and Transcribe activates a Loop.
bool modeSelects (SelectionKind kind, Mode mode)
{
    switch (kind)
    {
        case SelectionKind::clip:
        case SelectionKind::videoTake: return true;
        case SelectionKind::marker: return mode != Mode::record;
        case SelectionKind::loop: return mode == Mode::edit;
    }
    return false;
}
} // namespace

std::optional<song::Id> idOf (const std::optional<Selection>& selection, SelectionKind kind)
{
    if (selection.has_value() && selection->kind == kind)
        return selection->id;
    return std::nullopt;
}

std::optional<Selection> resolvedSelection (const std::optional<Selection>& held, const song::Song& song)
{
    if (held.has_value() && songHas (song, *held))
        return held;
    return std::nullopt;
}

std::optional<song::Id> selectedId (const std::optional<Selection>& held, SelectionKind kind, const song::Song& song)
{
    return idOf (resolvedSelection (held, song), kind);
}

std::optional<Selection> selectionAfterPick (const std::optional<Selection>& held, SelectionKind kind, const std::optional<song::Id>& picked, Mode mode)
{
    if (! picked.has_value())
    {
        // The empty Loop/Marker strip leaves a Clip or a Video Take selected.
        const auto stripClick = kind == SelectionKind::marker || kind == SelectionKind::loop;
        const auto heldOnStrip = idOf (held, SelectionKind::marker).has_value() || idOf (held, SelectionKind::loop).has_value();
        return stripClick && ! heldOnStrip ? held : std::nullopt;
    }
    if (! modeSelects (kind, mode))
        return held;
    return Selection { kind, *picked };
}

std::optional<Selection> deleteTarget (const std::optional<Selection>& held, const song::Song& song, Mode mode)
{
    const auto resolved = resolvedSelection (held, song);
    switch (mode)
    {
        case Mode::record: return std::nullopt;
        case Mode::edit: return resolved;
        case Mode::transcribe:
            if (idOf (resolved, SelectionKind::marker).has_value())
                return resolved;
            if (const auto* active = song::activeLoop (song))
                return Selection { SelectionKind::loop, active->id };
            return std::nullopt;
    }
    return std::nullopt;
}
} // namespace overdub::ui
