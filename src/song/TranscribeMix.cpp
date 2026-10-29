#include "song/TranscribeMix.h"

namespace overdub::song
{
core::Expected<Song> setFocusTrack (Song song, const std::optional<Id>& trackId)
{
    if (! trackId.has_value())
    {
        song.focusTrackId.reset();
        return song;
    }
    if (auto found = lookupTrack (song, *trackId); ! found.has_value())
        return std::unexpected (found.error());
    song.focusTrackId = trackId;
    for (auto& track : song.tracks)
        track.transcribeSolo = track.id == *trackId;
    return song;
}

core::Expected<Song> setTranscribeMute (Song song, const Id& trackId, bool mute)
{
    auto track = lookupTrack (song, trackId);
    if (! track.has_value())
        return std::unexpected (track.error());
    (*track)->transcribeMute = mute;
    return song;
}

core::Expected<Song> setTranscribeSolo (Song song, const Id& trackId, bool solo)
{
    auto track = lookupTrack (song, trackId);
    if (! track.has_value())
        return std::unexpected (track.error());
    (*track)->transcribeSolo = solo;
    return song;
}

Song withTranscribeMix (Song song)
{
    for (auto& track : song.tracks)
    {
        track.mute = track.transcribeMute;
        track.solo = track.transcribeSolo;
    }
    return song;
}

const Track* focusTrack (const Song& song)
{
    return song.focusTrackId.has_value() ? findTrack (song, *song.focusTrackId) : nullptr;
}
} // namespace overdub::song
