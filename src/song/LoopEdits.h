#pragma once

#include "core/Error.h"
#include "song/Song.h"

#include <optional>
#include <string>

// Loops (ADR 0010, issue #228): named ranges of the Song, as pure functions. Adding, resizing,
// renaming and deleting a Loop are each one undo step with its label (SongPackage::edit).
// Song::loops stays in time order: every function here leaves it sorted by start, Loops with one
// start in the order they were made or moved there. The active Loop, looping and each Loop's
// Practice Speed are outside history (setActiveLoop, setLooping, setLoopPracticeSpeed).
namespace overdub::song
{
inline constexpr auto addLoopLabel = "Add Loop";
inline constexpr auto resizeLoopLabel = "Resize Loop";
inline constexpr auto renameLoopLabel = "Rename Loop";
inline constexpr auto deleteLoopLabel = "Delete Loop";

enum class LoopDirection
{
    previous,
    next
};

// "Loop N" with the lowest N >= 1 that no Loop in the Song is named.
std::string nextLoopName (const Song& song);

// song.loop_not_found (loop_id): the only place that Error is built.
core::Error loopNotFound (const Id& loopId);

// The Loop with this id, or null.
const Loop* findLoop (const Song& song, const Id& loopId);

// The Song's active Loop, or null (none, or it is gone).
const Loop* activeLoop (const Song& song);

// The Song with a new Loop `loopId` named nextLoopName over the range between `from` and `to`
// (either order, clamped to 0 or later), after any Loop with the same start, drilled at
// `practiceSpeedPercent`. Errors: song.loop_empty (start, end): the range has no length;
// song.practice_speed_out_of_range (song/PracticeSpeed.h).
core::Expected<Song> addLoop (Song song, SampleCount from, SampleCount to, const Id& loopId, int practiceSpeedPercent);

// The Song with the Loop over the range between `from` and `to` (either order, clamped to 0 or
// later). Errors: song.loop_not_found (loop_id), song.loop_empty (start, end).
core::Expected<Song> resizeLoop (Song song, const Id& loopId, SampleCount from, SampleCount to);

// The Song with the Loop named `name` without its leading and trailing whitespace.
// Errors: song.loop_not_found (loop_id), song.loop_name_empty (loop_id).
core::Expected<Song> renameLoop (Song song, const Id& loopId, const std::string& name);

// The Song without the Loop; when it was active, none is and looping is off.
// Errors: song.loop_not_found (loop_id).
core::Expected<Song> deleteLoop (Song song, const Id& loopId);

// Outside history. The Song with this Loop active (none: no active Loop, looping off) and looping
// as asked. Errors: song.loop_not_found (loop_id).
core::Expected<Song> setActiveLoop (Song song, const std::optional<Id>& loopId, bool looping);

// Outside history. Errors: song.no_active_loop: looping on without an active Loop.
core::Expected<Song> setLooping (Song song, bool looping);

// Outside history. The Song with the Loop drilled at `practiceSpeedPercent`.
// Errors: song.loop_not_found (loop_id), song.practice_speed_out_of_range.
core::Expected<Song> setLoopPracticeSpeed (Song song, const Id& loopId, int practiceSpeedPercent);

// The Loop before or after `from` in time order: with none (or one the Song lacks), the first for
// next and the last for previous. None when there is no such Loop.
std::optional<Id> adjacentLoop (const Song& song, const std::optional<Id>& from, LoopDirection direction);
} // namespace overdub::song
