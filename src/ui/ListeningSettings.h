#pragma once

#include "core/Error.h"
#include "song/Song.h"
#include "ui/Intent.h"

// Transcribe mode's listening settings (issues #226, #233, #234, ADR 0010) as one function from a
// Song to the changed Song, like ui/ExportSettings.h. They are Song settings outside history: the
// caller stores the result with OpenSong::changeListening, never as an undo step. The checks are
// song/TranscribeMix.h's.
//
// - set_focus_track: song::setFocusTrack. Errors: song.track_not_found.
// - set_transcribe_mute, set_transcribe_solo: song::setTranscribeMute, song::setTranscribeSolo.
//   Errors: song.track_not_found.
// - set_octave_shift: song::setOctaveShift. Errors: song.octave_shift_out_of_range,
//   song.track_not_found.
// - set_band_filter: song::setBandFilter. Errors: song.band_filter_out_of_range,
//   song.band_filter_low_not_below_high, song.track_not_found.
namespace overdub::ui
{
core::Expected<song::Song> withListeningSetting (const song::Song& song, const intent::ListeningSetting& setting);

// Logs the setting once it is stored: ui.focus_track_set (info: track_id, "" for none),
// ui.transcribe_mute_set and ui.transcribe_solo_set (debug: track_id, mute / solo),
// ui.octave_shift_set (info: track_id, octaves), ui.band_filter_set (info: track_id, on, low_hz,
// high_hz).
void logListeningSet (const intent::ListeningSetting& setting);
} // namespace overdub::ui
