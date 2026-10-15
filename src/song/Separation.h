#pragma once

#include "core/Error.h"
#include "song/Song.h"

#include <array>
#include <string>
#include <string_view>
#include <vector>

// The Song change of a Separation (issue #114, ADR 0007) as pure functions. A Separation splits the
// one Backing Track a Track's Clips come from into six new Backing Tracks, each on its own new
// Track right below the source Track; the source Track is muted. SongPackage::addSeparation writes
// the parts' files and makes the change one undo step; song/SeparationRun.h drives the helper.
namespace overdub::song
{
// One part the helper writes: its file stem in the helper's output folder ("bass" -> bass.wav)
// and the name of the Track it lands on: an instrument emoji, a space and the part's name
// (issue #136), stored in the Song as UTF-8. The emoji are written as UTF-8 byte escapes so the
// source file's encoding cannot change them.
struct SeparationPart
{
    std::string_view fileStem;
    std::string_view trackName;
};

// The six parts in the helper's order (the model's `sources`), which is also the order of the new
// Tracks below the source Track.
inline constexpr std::array<SeparationPart, 6> separationParts { {
    { "bass", "\xf0\x9f\x8e\xbb Bass" }, // U+1F3BB violin
    { "drums", "\xf0\x9f\xa5\x81 Drums" }, // U+1F941 drum
    { "other", "\xf0\x9f\x8e\xb6 Other" }, // U+1F3B6 multiple musical notes
    { "vocals", "\xf0\x9f\x8e\xa4 Vocals" }, // U+1F3A4 microphone
    { "guitar", "\xf0\x9f\x8e\xb8 Guitar" }, // U+1F3B8 guitar
    { "piano", "\xf0\x9f\x8e\xb9 Piano" }, // U+1F3B9 musical keyboard
} };

// The Backing Track a Track can be separated from: the one every Clip of the Track plays.
// Errors: song.track_not_found (track_id), song.track_not_separable (track_id, reason: "no_clips"
// when the Track has no Clip, "take_clips" when a Clip plays a Take, "several_backing_tracks" when
// the Clips play different Backing Tracks, "backing_track_missing" when the Song lacks it).
core::Expected<Id> separableBackingTrack (const Song& song, const Id& trackId);

// The Song with the Separation applied: `parts` (one Backing Track per separationParts entry, in
// that order) added to the Song's Backing Tracks; one new Track per part inserted directly below
// the source Track, named after the part, coloured newTrackColour (each new Track's colour taken
// for the next, so the six differ while the palette allows), at clean mixer defaults (0 dB, centre pan, no Insert
// Chain, not muted, soloed or Armed), holding a copy of every source Track Clip (same timeline
// start, source offset, length, gain and fades; a new id) that plays the part; the source Track
// muted and otherwise unchanged.
// Errors: separableBackingTrack's, song.separation_parts_invalid (expected, found: not one
// Backing Track per part).
core::Expected<Song> withSeparation (Song song, const Id& sourceTrackId, const std::vector<BackingTrack>& parts, const PickIndex& pickColour);

// The undo step label of a Separation: "Separate <original filename>" of the source Backing Track.
std::string separationStepLabel (const BackingTrack& source);
} // namespace overdub::song
