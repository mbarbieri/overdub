#include "song/VideoTakeEdits.h"

#include <algorithm>

namespace overdub::song
{
const VideoTake* findVideoTake (const Song& song, const Id& videoTakeId)
{
    const auto found = std::ranges::find (song.videoTakes, videoTakeId, &VideoTake::id);
    return found != song.videoTakes.end() ? &*found : nullptr;
}

core::Expected<Song> deleteVideoTake (Song song, const Id& videoTakeId)
{
    const auto found = std::ranges::find (song.videoTakes, videoTakeId, &VideoTake::id);
    if (found == song.videoTakes.end())
        return std::unexpected (core::Error { "song.video_take_not_found", "no Video Take with this id in the Song", { { "video_take_id", videoTakeId } } });
    song.videoTakes.erase (found);
    if (song.settings.exportVideoTakeId == videoTakeId)
        song.settings.exportVideoTakeId.reset();
    return song;
}
} // namespace overdub::song
