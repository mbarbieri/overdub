#pragma once

#include "core/Error.h"
#include "song/Song.h"
#include "ui/Intent.h"

// The Export sheet's choices (issues #201, #235) as one function from a Song to the changed Song,
// the way ui/SongEdits.h pairs each history edit with its Song. They are Song settings outside
// history: the caller stores the result with OpenSong::changeSettings, never as an undo step.
// Every check lives here, because the same Intent arrives from the sheet and from the socket.
//
// - set_export_format and set_export_length's MP4 length are stored as given.
// - set_export_length's Loop is the one whose range every format writes; none is the whole Song.
//   Errors: song.loop_not_found (loop_id).
// - set_export_destination stores the path as juce::File spells it. Errors: export.folder_missing
//   (the file's folder does not exist; it is not created).
// - set_export_video_take ignores an id that is not a Video Take of the Song.
namespace overdub::ui
{
core::Expected<song::Song> withExportSetting (song::Song song, const intent::ExportSetting& setting);
} // namespace overdub::ui
