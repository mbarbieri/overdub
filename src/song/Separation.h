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
// and the name of the Track it lands on.
struct SeparationPart
{
    std::string_view fileStem;
    std::string_view trackName;
};

// The six parts in the helper's order (the model's `sources`), which is also the order of the new
// Tracks below the source Track.
inline constexpr std::array<SeparationPart, 6> separationParts { {
    { "bass", "Bass" },
    { "drums", "Drums" },
    { "other", "Other" },
    { "vocals", "Vocals" },
    { "guitar", "Guitar" },
    { "piano", "Piano" },
} };

// The Backing Track a Track can be separated from: the one every Clip of the Track plays.
// Errors: song.track_not_found (track_id), song.track_not_separable (track_id, reason: "no_clips"
// when the Track has no Clip, "not_one_backing_track" when a Clip plays a Take or the Clips play
// different Backing Tracks).
core::Expected<Id> separableBackingTrack (const Song& song, const Id& trackId);

// The Song with the Separation applied: `parts` (one Backing Track per separationParts entry, in
// that order) added to the Song's Backing Tracks; one new Track per part inserted directly below
// the source Track, named after the part, at clean mixer defaults (0 dB, centre pan, no Insert
// Chain, not muted, soloed or Armed), holding a copy of every source Track Clip (same timeline
// start, source offset, length, gain and fades; a new id) that plays the part; the source Track
// muted and otherwise unchanged.
// Errors: separableBackingTrack's, song.separation_parts_invalid (expected, found: not one
// Backing Track per part).
core::Expected<Song> withSeparation (Song song, const Id& sourceTrackId, const std::vector<BackingTrack>& parts);

// The undo step label of a Separation: "Separate <original filename>" of the source Backing Track.
std::string separationStepLabel (const BackingTrack& source);
} // namespace overdub::song
