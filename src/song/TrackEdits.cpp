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
} // namespace overdub::song
