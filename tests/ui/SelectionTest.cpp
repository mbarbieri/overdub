#include "ui/Selection.h"

#include "song/FullSong.h"

#include <catch2/catch_test_macros.hpp>

using overdub::song::Song;
using overdub::ui::deleteTarget;
using overdub::ui::Mode;
using overdub::ui::resolvedSelection;
using overdub::ui::Selection;
using overdub::ui::selectionAfterPick;
using overdub::ui::SelectionKind;

// The Selection as a value: no window, no Timeline. The Song is fullyPopulatedSong (Clips clip-a..d,
// Video Takes video-1 and video-2, Markers marker-1 and marker-2, Loops loop-1 and loop-2, loop-2
// active).
namespace
{
const Selection clip { SelectionKind::clip, "clip-a" };
const Selection videoTake { SelectionKind::videoTake, "video-1" };
const Selection marker { SelectionKind::marker, "marker-1" };
const Selection loop { SelectionKind::loop, "loop-1" };
const Selection activeLoop { SelectionKind::loop, "loop-2" };
} // namespace

TEST_CASE ("selecting a Clip, a Video Take, a Marker or a Loop in Edit mode clears whatever was selected", "[ui][selection]")
{
    for (const auto& held : { std::optional<Selection> {}, std::optional (clip), std::optional (videoTake), std::optional (marker), std::optional (loop) })
    {
        CHECK (selectionAfterPick (held, SelectionKind::clip, "clip-b", Mode::edit) == Selection { SelectionKind::clip, "clip-b" });
        CHECK (selectionAfterPick (held, SelectionKind::videoTake, "video-2", Mode::edit) == Selection { SelectionKind::videoTake, "video-2" });
        CHECK (selectionAfterPick (held, SelectionKind::marker, "marker-2", Mode::edit) == Selection { SelectionKind::marker, "marker-2" });
        CHECK (selectionAfterPick (held, SelectionKind::loop, "loop-2", Mode::edit) == Selection { SelectionKind::loop, "loop-2" });
    }
}

TEST_CASE ("Record mode never selects a Marker or a Loop, and Transcribe mode never selects a Loop", "[ui][selection]")
{
    CHECK_FALSE (selectionAfterPick (std::nullopt, SelectionKind::marker, "marker-2", Mode::record).has_value());
    CHECK_FALSE (selectionAfterPick (std::nullopt, SelectionKind::loop, "loop-2", Mode::record).has_value());
    CHECK_FALSE (selectionAfterPick (std::nullopt, SelectionKind::loop, "loop-2", Mode::transcribe).has_value());
    // Picking them there seeks, or activates the Loop: what was selected stays.
    CHECK (selectionAfterPick (clip, SelectionKind::marker, "marker-2", Mode::record) == clip);
    CHECK (selectionAfterPick (clip, SelectionKind::loop, "loop-2", Mode::record) == clip);
    CHECK (selectionAfterPick (marker, SelectionKind::loop, "loop-2", Mode::transcribe) == marker);
    CHECK (selectionAfterPick (clip, SelectionKind::marker, "marker-2", Mode::transcribe) == Selection { SelectionKind::marker, "marker-2" });
}

TEST_CASE ("a Clip or a Video Take is selected in every mode", "[ui][selection]")
{
    for (const auto mode : { Mode::record, Mode::transcribe })
    {
        CHECK (selectionAfterPick (marker, SelectionKind::clip, "clip-b", mode) == Selection { SelectionKind::clip, "clip-b" });
        CHECK (selectionAfterPick (marker, SelectionKind::videoTake, "video-2", mode) == Selection { SelectionKind::videoTake, "video-2" });
    }
}

TEST_CASE ("a click on nothing clears the Selection, and an empty strip clears only a Marker or a Loop", "[ui][selection]")
{
    for (const auto mode : { Mode::record, Mode::edit, Mode::transcribe })
    {
        CHECK_FALSE (selectionAfterPick (marker, SelectionKind::clip, std::nullopt, mode).has_value());
        CHECK_FALSE (selectionAfterPick (loop, SelectionKind::videoTake, std::nullopt, mode).has_value());
        CHECK_FALSE (selectionAfterPick (marker, SelectionKind::marker, std::nullopt, mode).has_value());
        CHECK_FALSE (selectionAfterPick (loop, SelectionKind::loop, std::nullopt, mode).has_value());
        CHECK (selectionAfterPick (clip, SelectionKind::marker, std::nullopt, mode) == clip);
        CHECK (selectionAfterPick (videoTake, SelectionKind::loop, std::nullopt, mode) == videoTake);
    }
}

TEST_CASE ("a selected id the Song no longer has reads as none, and the Song having it again brings it back", "[ui][selection]")
{
    const auto song = overdub::song::testing::fullyPopulatedSong();
    for (const auto& held : { clip, videoTake, marker, loop })
        CHECK (resolvedSelection (held, song) == held);

    auto without = song;
    without.tracks[0].clips.clear();
    without.tracks[1].clips.clear();
    without.videoTakes.clear();
    without.markers.clear();
    without.loops.clear();
    without.settings.activeLoopId.reset();
    for (const auto& held : { clip, videoTake, marker, loop })
        CHECK_FALSE (resolvedSelection (held, without).has_value());
    CHECK_FALSE (resolvedSelection (std::nullopt, song).has_value());
}

TEST_CASE ("what Backspace deletes: nothing in Record, the Selection in Edit, the Marker or else the active Loop in Transcribe", "[ui][selection]")
{
    const auto song = overdub::song::testing::fullyPopulatedSong();

    for (const auto& held : { clip, videoTake, marker, loop })
    {
        CHECK_FALSE (deleteTarget (held, song, Mode::record).has_value());
        CHECK (deleteTarget (held, song, Mode::edit) == held);
    }
    CHECK_FALSE (deleteTarget (std::nullopt, song, Mode::edit).has_value());

    CHECK (deleteTarget (marker, song, Mode::transcribe) == marker);
    CHECK (deleteTarget (clip, song, Mode::transcribe) == activeLoop);
    CHECK (deleteTarget (loop, song, Mode::transcribe) == activeLoop);
    CHECK (deleteTarget (std::nullopt, song, Mode::transcribe) == activeLoop);

    auto noActiveLoop = song;
    noActiveLoop.settings.activeLoopId.reset();
    CHECK_FALSE (deleteTarget (clip, noActiveLoop, Mode::transcribe).has_value());

    // A Marker the Song lost is not deleted; the active Loop is, in Transcribe.
    auto noMarkers = song;
    noMarkers.markers.clear();
    CHECK_FALSE (deleteTarget (marker, noMarkers, Mode::edit).has_value());
    CHECK (deleteTarget (marker, noMarkers, Mode::transcribe) == activeLoop);
}
