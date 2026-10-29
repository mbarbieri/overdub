#pragma once

#include "core/Error.h"
#include "song/Song.h"

#include <optional>

// Transcribe mode's Focus Track, Transcribe Mix (ADR 0010, issue #226) and each Track's octave
// shift (issue #233) and band filter (issue #234) as pure functions. All are Song state outside
// history (Song::focusTrackId, Track::transcribeMute / transcribeSolo / octaveShift / bandFilter):
// the caller stores the returned Song with SongPackage::changeWithoutHistory, never as an undo
// step. What Transcribe mode hears is withTranscribeMix; the Song's own mix never changes.
// Errors: song.track_not_found (track_id).
namespace overdub::song
{
inline constexpr int minOctaveShift = -1;
inline constexpr int maxOctaveShift = 2;

// The Focus Track picked: it is soloed in the Transcribe Mix and every other Track unsoloed;
// mutes stay. None clears the Focus Track and leaves the Transcribe Mix as it is.
core::Expected<Song> setFocusTrack (Song song, const std::optional<Id>& trackId);
core::Expected<Song> setTranscribeMute (Song song, const Id& trackId, bool mute);
core::Expected<Song> setTranscribeSolo (Song song, const Id& trackId, bool solo);
// The Track's own octave shift, kept when the Focus moves to another Track and back.
// Errors: song.octave_shift_out_of_range (octaves, min, max), song.track_not_found.
core::Expected<Song> setOctaveShift (Song song, const Id& trackId, int octaves);
// The Track's own band filter, kept like its octave shift. Both cuts are within
// minBandFilterHz..maxBandFilterHz and the low cut is below the high cut, whether on or off.
// Errors: song.band_filter_out_of_range (low_hz, high_hz, min_hz, max_hz),
// song.band_filter_low_not_below_high (low_hz, high_hz), song.track_not_found.
core::Expected<Song> setBandFilter (Song song, const Id& trackId, BandFilter filter);

// The Song as Transcribe mode hears it: every Track's mute and solo replaced by its Transcribe
// Mix's. Volumes, pans and Insert Chains stay the Song's.
Song withTranscribeMix (Song song);

// The Focus Track, or null when there is none (or its id names no Track).
const Track* focusTrack (const Song& song);

// The octave shift Transcribe mode hears: the Focus Track's, 0 when there is none.
int focusOctaveShift (const Song& song);
} // namespace overdub::song
