#include "song/LoopEdits.h"

#include "song/MixEdits.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace overdub::song;

// Loops (issue #228, ADR 0010) as pure Song functions: "Loop N" names, time order by start, the
// active Loop and looping outside history, and the Errors of a missing Loop, an empty range or name.
namespace
{
std::vector<std::string> namesInOrder (const Song& song)
{
    std::vector<std::string> names;
    for (const auto& loop : song.loops)
        names.push_back (loop.name);
    return names;
}

Song songWithLoops()
{
    auto song = makeEmptySong (48000);
    song = *addLoop (song, 48000, 96000, "loop-b");
    song = *addLoop (song, 0, 24000, "loop-a");
    song = *addLoop (song, 120000, 100000, "loop-c");
    return song;
}
} // namespace

TEST_CASE ("a new Loop is named Loop N with the lowest unused N, spans its range in either order and keeps time order", "[song][loops]")
{
    auto song = songWithLoops();

    CHECK (namesInOrder (song) == std::vector<std::string> { "Loop 2", "Loop 1", "Loop 3" });
    CHECK (song.loops[2].start == 100000);
    CHECK (song.loops[2].end == 120000);
    CHECK (song.loops[2].length() == 20000);

    song = *renameLoop (song, "loop-b", "  Chorus ");
    CHECK (song.loops[1].name == "Chorus");
    CHECK (nextLoopName (song) == "Loop 1");

    // Clamped to 0 or later.
    const auto clamped = addLoop (song, -500, 300, "loop-d");
    REQUIRE (clamped.has_value());
    CHECK (findLoop (*clamped, "loop-d")->start == 0);
    CHECK (findLoop (*clamped, "loop-d")->end == 300);
}

TEST_CASE ("resizing a Loop moves it into time order; an empty range, a missing Loop or a blank name is refused", "[song][loops]")
{
    const auto song = songWithLoops();

    const auto resized = resizeLoop (song, "loop-a", 130000, 110000);
    REQUIRE (resized.has_value());
    CHECK (namesInOrder (*resized) == std::vector<std::string> { "Loop 1", "Loop 3", "Loop 2" });
    CHECK (findLoop (*resized, "loop-a")->start == 110000);
    CHECK (findLoop (*resized, "loop-a")->end == 130000);

    CHECK (resizeLoop (song, "loop-a", 500, 500).error().code == "song.loop_empty");
    CHECK (addLoop (song, 500, 500, "loop-e").error().code == "song.loop_empty");
    CHECK (resizeLoop (song, "loop-gone", 0, 10).error().code == "song.loop_not_found");
    CHECK (resizeLoop (song, "loop-gone", 0, 10).error().fields.at ("loop_id") == "loop-gone");
    CHECK (renameLoop (song, "loop-a", "   ").error().code == "song.loop_name_empty");
    CHECK (renameLoop (song, "loop-gone", "x").error().code == "song.loop_not_found");
    CHECK (deleteLoop (song, "loop-gone").error().code == "song.loop_not_found");
}

TEST_CASE ("the active Loop and looping: deleting the active Loop turns looping off; looping needs an active Loop", "[song][loops]")
{
    auto song = songWithLoops();
    CHECK (setLooping (song, true).error().code == "song.no_active_loop");
    CHECK (setActiveLoop (song, "loop-gone", true).error().code == "song.loop_not_found");

    song = *setActiveLoop (song, "loop-b", true);
    CHECK (song.activeLoopId == "loop-b");
    CHECK (song.looping);
    CHECK (activeLoop (song)->name == "Loop 1");
    song = *setLooping (song, false);
    CHECK_FALSE (song.looping);
    song = *setLooping (song, true);

    // Deleting another Loop keeps it; deleting it clears both.
    CHECK (deleteLoop (song, "loop-a")->activeLoopId == "loop-b");
    const auto deleted = deleteLoop (song, "loop-b");
    REQUIRE (deleted.has_value());
    CHECK_FALSE (deleted->activeLoopId.has_value());
    CHECK_FALSE (deleted->looping);
}

TEST_CASE ("the previous and next Loop go by time order and stop at the ends", "[song][loops]")
{
    const auto song = songWithLoops(); // loop-a, loop-b, loop-c

    CHECK (adjacentLoop (song, std::nullopt, LoopDirection::next) == "loop-a");
    CHECK (adjacentLoop (song, std::nullopt, LoopDirection::previous) == "loop-c");
    CHECK (adjacentLoop (song, "loop-a", LoopDirection::next) == "loop-b");
    CHECK (adjacentLoop (song, "loop-b", LoopDirection::previous) == "loop-a");
    CHECK_FALSE (adjacentLoop (song, "loop-c", LoopDirection::next).has_value());
    CHECK_FALSE (adjacentLoop (song, "loop-a", LoopDirection::previous).has_value());
    CHECK_FALSE (adjacentLoop (makeEmptySong (48000), std::nullopt, LoopDirection::next).has_value());
}

TEST_CASE ("a change only to the Loops or looping plays on without a new plan", "[song][loops]")
{
    const auto song = songWithLoops();
    auto changed = *setActiveLoop (*resizeLoop (song, "loop-a", 10, 20), "loop-a", true);

    CHECK (differsOnlyInMix (song, changed));
}

TEST_CASE ("undo and redo keep the active Loop and looping, unless the restored Song lacks that Loop", "[song][loops][history]")
{
    const auto before = makeEmptySong (48000);
    const auto withLoop = *setActiveLoop (*addLoop (before, 0, 100, "loop-a"), "loop-a", true);

    const auto undone = keepStateOutsideHistory (before, withLoop);
    CHECK_FALSE (undone.activeLoopId.has_value());
    CHECK_FALSE (undone.looping);

    const auto renamed = *renameLoop (withLoop, "loop-a", "Riff");
    const auto restored = keepStateOutsideHistory (*addLoop (before, 0, 100, "loop-a"), renamed);
    CHECK (restored.activeLoopId == "loop-a");
    CHECK (restored.looping);
}
