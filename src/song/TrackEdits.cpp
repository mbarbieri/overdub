#include "song/TrackEdits.h"

#include <algorithm>

namespace overdub::song
{
std::string nextTrackName (const Song& song)
{
    for (int number = 1;; ++number)
    {
        auto name = "Track " + std::to_string (number);
        if (std::ranges::find (song.tracks, name, &Track::name) == song.tracks.end())
            return name;
    }
}

AddedTrack addTrack (Song song, std::vector<int> inputChannels)
{
    Track track;
    track.id = makeId();
    track.name = nextTrackName (song);
    track.armed = true;
    track.inputChannels = std::move (inputChannels);

    AddedTrack added;
    added.trackId = track.id;
    added.label = "Add " + track.name;
    song.tracks.push_back (std::move (track));
    added.song = std::move (song);
    return added;
}
core::Expected<DeletedTrack> deleteTrack (Song song, const Id& trackId)
{
    const auto found = std::ranges::find (song.tracks, trackId, &Track::id);
    if (found == song.tracks.end())
        return std::unexpected (core::Error { "song.track_not_found", "no Track with this id in the Song", { { "track_id", trackId } } });
    auto label = "Delete Track " + found->name;
    song.tracks.erase (found);
    return DeletedTrack { std::move (song), std::move (label) };
}
} // namespace overdub::song
