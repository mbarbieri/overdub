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

AddedTrack addTrack (Song song, std::vector<int> inputChannels, const PickIndex& pickColour)
{
    Track track;
    track.id = makeId();
    track.name = nextTrackName (song);
    track.colour = newTrackColour (trackColours (song), pickColour);
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
    const auto track = lookupTrack (song, trackId);
    if (! track.has_value())
        return std::unexpected (track.error());
    auto label = "Delete Track " + (*track)->name;
    song.tracks.erase (song.tracks.begin() + (*track - song.tracks.data()));
    return DeletedTrack { std::move (song), std::move (label) };
}

core::Expected<std::optional<MovedTrack>> moveTrack (Song song, const Id& trackId, std::size_t toIndex)
{
    const auto found = lookupTrack (song, trackId);
    if (! found.has_value())
        return std::unexpected (found.error());
    const auto from = (std::size_t) (*found - song.tracks.data());
    const auto to = std::min (toIndex, song.tracks.size() - 1);
    if (to == from)
        return std::optional<MovedTrack> {};
    auto track = std::move (**found);
    song.tracks.erase (song.tracks.begin() + (std::ptrdiff_t) from);
    song.tracks.insert (song.tracks.begin() + (std::ptrdiff_t) to, std::move (track));
    return std::optional<MovedTrack> { MovedTrack { std::move (song), moveTrackLabel } };
}
} // namespace overdub::song
