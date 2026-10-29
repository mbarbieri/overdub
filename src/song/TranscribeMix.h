#pragma once

#include "core/Error.h"
#include "song/Song.h"

#include <optional>

// Transcribe mode's Focus Track and Transcribe Mix (ADR 0010, issue #226) as pure functions. Both
// are Song state outside history (Song::focusTrackId, Track::transcribeMute / transcribeSolo):
// the caller stores the returned Song with SongPackage::changeWithoutHistory, never as an undo
// step. What Transcribe mode hears is withTranscribeMix; the Song's own mix never changes.
// Errors: song.track_not_found (track_id).
namespace overdub::song
{
// The Focus Track picked: it is soloed in the Transcribe Mix and every other Track unsoloed;
// mutes stay. None clears the Focus Track and leaves the Transcribe Mix as it is.
core::Expected<Song> setFocusTrack (Song song, const std::optional<Id>& trackId);
core::Expected<Song> setTranscribeMute (Song song, const Id& trackId, bool mute);
core::Expected<Song> setTranscribeSolo (Song song, const Id& trackId, bool solo);

// The Song as Transcribe mode hears it: every Track's mute and solo replaced by its Transcribe
// Mix's. Volumes, pans and Insert Chains stay the Song's.
Song withTranscribeMix (Song song);

// The Focus Track, or null when there is none (or its id names no Track).
const Track* focusTrack (const Song& song);
} // namespace overdub::song
