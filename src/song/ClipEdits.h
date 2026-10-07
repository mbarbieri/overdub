#pragma once

#include "core/Error.h"
#include "song/Song.h"

#include <vector>

// The Edit timeline's Clip operations (issue #28) as pure functions over the Song: move, trim
// start, trim end, split and delete. Each returns the changed Song; the caller makes it one undo
// step with its label (SongPackage::edit). Integer samples throughout (ADR 0004).
//
// Invariants every operation keeps:
// - A Clip keeps its place in its Track's layer order (the last Clip is on top); a split puts both
//   halves where the Clip was, the right half just above the left one.
// - A Clip starts at sample 0 or later, reads its source from sample 0 or later, is at least
//   one sample long, and never reads past the end of its source.
// - Fades are clamped into the Clip's length (fade-in first).
// Errors: song.clip_not_found (clip_id), song.split_outside_clip (clip_id, at).
namespace overdub::song
{
inline constexpr const char* moveClipLabel = "Move Clip";
inline constexpr const char* trimClipLabel = "Trim Clip";
inline constexpr const char* splitClipLabel = "Split Clip";
inline constexpr const char* deleteClipLabel = "Delete Clip";

// A Song sample range [start, end).
struct SampleSpan
{
    SampleCount start = 0;
    SampleCount end = 0;

    bool operator== (const SampleSpan&) const = default;
};

// The song.clip_not_found Error (clip_id) the operations return for an unknown Clip id.
core::Error clipNotFound (const Id& clipId);

// The Clip with this id on any Track, or null.
const Clip* findClip (const Song& song, const Id& clipId);
// The Track holding the Clip with this id, or null.
const Track* trackOfClip (const Song& song, const Id& clipId);

// The length in samples of the Take or Backing Track a Clip reads; 0 when the Song lacks it.
SampleCount sourceLength (const Song& song, const ClipSource& source);

// The Clip at a new timeline start (clamped to 0 or later). Length and source offset unchanged.
core::Expected<Song> moveClip (Song song, const Id& clipId, SampleCount newTimelineStart);

// The Clip's start edge moved to `newTimelineStart`, its end kept: the source offset moves by the
// same amount. Clamped so the source offset and start stay >= 0 and the length >= 1.
core::Expected<Song> trimClipStart (Song song, const Id& clipId, SampleCount newTimelineStart);

// The Clip's end edge moved to `newTimelineEnd`, its start kept. Clamped so the length stays >= 1
// and the Clip does not read past its source's end (a source the Song lacks: no longer than now).
core::Expected<Song> trimClipEnd (Song song, const Id& clipId, SampleCount newTimelineEnd);

// Splits the Clip at Song sample `at`, strictly inside it. The left half keeps the id and fade-in,
// the right half gets `rightClipId` and the fade-out; gain is kept on both.
core::Expected<Song> splitClip (Song song, const Id& clipId, SampleCount at, const Id& rightClipId);

// Removes the Clip. Its Take or Backing Track stays in the Song (media is never deleted).
core::Expected<Song> deleteClip (Song song, const Id& clipId);

// Where a higher Clip hides part of a lower one on this Track (the hatched strip), merged and
// sorted by start.
std::vector<SampleSpan> coveredSpans (const Track& track);
} // namespace overdub::song
