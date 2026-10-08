#pragma once

#include "core/Error.h"
#include "song/ClipEdits.h"
#include "song/Song.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Alternate Takes (issue #30, ADR 0004 amendment for issue #15) as pure functions over the Song:
// which Takes a Track's Take Lanes list, where each Take is heard, a Pass's heard status, Swap
// (the Clip plays another Take), and bringing a Take or a whole Pass back on top. Each operation
// returns the changed Song; the caller makes it one undo step with its label (SongPackage::edit).
//
// A Take's audio is its recorded span: the Song samples its Clip covered when the Pass ended,
// from the Pass start (or the Take's origin, if later) to the Take's end (capture/PassRecorder.h).
// Swap, bring back and heard status all measure against that span.
//
// Errors: song.clip_not_found (clip_id), song.take_not_found (take_id), song.pass_not_found
// (pass_id), song.take_not_under_clip (clip_id, take_id), song.take_not_on_clip_track (clip_id, take_id), song.take_track_missing (take_id,
// track_id), song.take_has_no_audio (take_id), song.pass_has_no_takes (pass_id).
namespace overdub::song
{
enum class HeardStatus
{
    heard,       // every recorded sample of every surviving Take of the Pass is heard
    partlyHeard, // some, not all
    notHeard     // none (also a Pass with no surviving Take)
};

// "heard", "partly heard", "not heard".
std::string_view toString (HeardStatus status);

// The Pass's number, from 1 (its place in song.passes); 0 when the Song lacks it.
int passNumber (const Song& song, const Id& passId);

const Take* findTake (const Song& song, const Id& takeId);
const Track* findTrack (const Song& song, const Id& trackId);

// The Clip that plays a Take as recorded: from passStart (or the Take's origin, if later) to the
// end of the Take, reading the Take where that Song sample belongs; gain 0 dB, no fades. None when
// that span is empty.
std::optional<Clip> clipAsRecorded (const Take& take, SampleCount passStart, const Id& clipId);

// The Take's recorded span (see above), its Pass's start looked up in the Song (the Take's origin
// when the Song lacks the Pass). None when it is empty or the Song lacks the Take.
std::optional<SampleSpan> recordedSpan (const Song& song, const Id& takeId);

// The Clip that sounds at Song sample `at` on this Track: the top (last) Clip covering it, or null.
const Clip* topClipAt (const Track& track, SampleCount at);

// Where the Take is heard now: the spans of its Track where the top Clip plays it, merged and
// sorted by start. Empty when its Track is gone.
std::vector<SampleSpan> heardSpans (const Song& song, const Id& takeId);

// Whether the Take's recorded span is heard: all of it, part of it, or none (also when the Song
// lacks the Take, its Track is gone, or the span is empty).
HeardStatus takeHeardStatus (const Song& song, const Id& takeId);

// Over every Take of the Pass whose Track still exists: heard when all of their recorded spans are
// heard, not heard when none is, partly heard otherwise.
HeardStatus passHeardStatus (const Song& song, const Id& passId);

// The Takes a Track's Take Lanes list: those recorded on it (Take::trackId), newest Pass first.
// Takes whose Pass the Song lacks come last.
std::vector<const Take*> laneTakes (const Song& song, const Id& trackId);

// Swap: the Clip plays this Take instead. It keeps its layer, gain and fades; it is trimmed to
// where its span and the Take's recorded span overlap, the source offset is recomputed so the
// Take stays where it was recorded (timeline start - origin), and the fades are clamped to the
// new length (fade-in first). Errors: song.clip_not_found, song.take_not_found,
// song.take_not_on_clip_track (recorded on another Track), song.take_not_under_clip (no overlap).
core::Expected<Song> swapClipTake (Song song, const Id& clipId, const Id& takeId);

// The whole Take back as recorded, as a new Clip (with this id) on the top layer of its Track.
// Errors: song.take_not_found, song.take_track_missing, song.take_has_no_audio.
core::Expected<Song> bringBackTake (Song song, const Id& takeId, const Id& newClipId);

// Every Take of the Pass back as recorded: one new top-layer Clip per Take on its Track, skipping
// Takes whose Track is gone or whose recorded span is empty. New Clip ids from makeId().
// Errors: song.pass_not_found, song.pass_has_no_takes (no Clip would be added).
core::Expected<Song> bringBackPass (Song song, const Id& passId);

// Whether bringBackPass would add a Clip: some Take of the Pass is on a Track that still exists
// and has audio from its Pass start.
bool canBringBackPass (const Song& song, const Id& passId);

// Undo step labels: "Use Pass 2 on Guitar", "Bring back Pass 2 on Guitar", "Bring back Pass 2".
std::string useTakeLabel (const Song& song, const Take& take);
std::string bringBackTakeLabel (const Song& song, const Take& take);
std::string bringBackPassLabel (const Song& song, const Id& passId);
} // namespace overdub::song
