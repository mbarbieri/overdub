#pragma once

#include "core/Error.h"
#include "song/Song.h"

// Deleting a Video Take from the Edit Timeline (issue #128) as a pure function over the Song. The
// caller makes the returned Song one undo step labelled deleteVideoTakeLabel (SongPackage::edit).
// Media is never deleted (ADR 0004): the movie file stays in the package, and the Pass and its
// Takes stay in the Song.
namespace overdub::song
{
inline constexpr const char* deleteVideoTakeLabel = "Delete Video Take";

// The Video Take with this id, or null.
const VideoTake* findVideoTake (const Song& song, const Id& videoTakeId);

// The Song without the Video Take. When the export's chosen Video Take (SongSettings::exportVideoTakeId) is
// the removed one, the choice is cleared so the Export page falls back to its default.
// Errors: song.video_take_not_found (video_take_id).
core::Expected<Song> deleteVideoTake (Song song, const Id& videoTakeId);
} // namespace overdub::song
