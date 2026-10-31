#pragma once

#include "core/Error.h"
#include "song/Song.h"
#include "ui/Intent.h"

// Every Song setting outside history (intent::SongSetting) as one function from a Song to the
// changed Song, the way ui/SongEdits.h pairs each plain edit with its Song. The caller stores the
// result with OpenSong::changeSettings, never as an undo step. Every check on the Song lives here,
// because the same Intent arrives from a page and from the socket.
//
// - set_armed, set_input_channels: that Track's setting. An unknown Track id changes nothing.
//   Whether the device offers the input channels is the caller's check (the Song cannot tell).
// - set_pre_roll: stored as given.
// - set_looping: song::setLooping. Errors: song.no_active_loop.
// - set_focus_track, set_transcribe_mute, set_transcribe_solo, set_octave_shift, set_band_filter:
//   song/TranscribeMix.h's. Errors: song.track_not_found, song.octave_shift_out_of_range,
//   song.band_filter_out_of_range, song.band_filter_low_not_below_high.
// - set_export_format and set_export_length's MP4 length are stored as given.
// - set_export_length's Loop is the one whose range every format writes; none is the whole Song.
//   Errors: song.loop_not_found (loop_id).
// - set_export_destination stores the path as juce::File spells it. Errors: export.folder_missing
//   (the file's folder does not exist; it is not created).
// - set_export_video_take ignores an id that is not a Video Take of the Song.
namespace overdub::ui
{
core::Expected<song::Song> withSongSetting (song::Song song, const intent::SongSetting& setting);

// Logs the setting once it changed the Song: ui.looping_set (info: looping), ui.focus_track_set
// (info: track_id, "" for none), ui.transcribe_mute_set and ui.transcribe_solo_set (debug: track_id,
// mute / solo), ui.octave_shift_set (info: track_id, octaves), ui.band_filter_set (info: track_id,
// on, low_hz, high_hz). The other settings log nothing.
void logSongSettingSet (const intent::SongSetting& setting);
} // namespace overdub::ui
