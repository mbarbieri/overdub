#include "song/MixEdits.h"

#include <algorithm>

namespace overdub::song
{
namespace
{
std::string trackName (const Song& song, const Id& trackId)
{
    const auto* track = findTrack (song, trackId);
    return track != nullptr ? track->name : "Track";
}

// The Song with `change` applied to that Track, or song.track_not_found.
template <typename Change>
core::Expected<Song> changeTrack (Song song, const Id& trackId, Change change)
{
    const auto track = lookupTrack (song, trackId);
    if (! track.has_value())
        return std::unexpected (track.error());
    change (**track);
    return song;
}

// The Song with every mix setting taken from `from`. Callers check the Track counts match first.
Song withMixOf (Song song, const Song& from)
{
    for (std::size_t i = 0; i < song.tracks.size() && i < from.tracks.size(); ++i)
    {
        song.tracks[i].volumeDb = from.tracks[i].volumeDb;
        song.tracks[i].pan = from.tracks[i].pan;
        song.tracks[i].mute = from.tracks[i].mute;
        song.tracks[i].solo = from.tracks[i].solo;
        song.tracks[i].transcribeMute = from.tracks[i].transcribeMute;
        song.tracks[i].transcribeSolo = from.tracks[i].transcribeSolo;
    }
    song.master.volumeDb = from.master.volumeDb;
    song.focusTrackId = from.focusTrackId;
    return song;
}
} // namespace

core::Expected<Song> setTrackVolume (Song song, const Id& trackId, double volumeDb)
{
    return changeTrack (std::move (song), trackId, [&] (Track& track) { track.volumeDb = std::clamp (volumeDb, minVolumeDb, maxVolumeDb); });
}

core::Expected<Song> setTrackPan (Song song, const Id& trackId, double pan)
{
    return changeTrack (std::move (song), trackId, [&] (Track& track) { track.pan = std::clamp (pan, -1.0, 1.0); });
}

core::Expected<Song> setTrackMute (Song song, const Id& trackId, bool mute)
{
    return changeTrack (std::move (song), trackId, [&] (Track& track) { track.mute = mute; });
}

core::Expected<Song> setTrackSolo (Song song, const Id& trackId, bool solo)
{
    return changeTrack (std::move (song), trackId, [&] (Track& track) { track.solo = solo; });
}

Song setMasterVolume (Song song, double volumeDb)
{
    song.master.volumeDb = std::clamp (volumeDb, minVolumeDb, maxVolumeDb);
    return song;
}

std::string volumeLabel (const Song& song, const Id& trackId)
{
    return trackName (song, trackId) + " Volume";
}

std::string panLabel (const Song& song, const Id& trackId)
{
    return trackName (song, trackId) + " Pan";
}

std::string muteLabel (const Song& song, const Id& trackId, bool mute)
{
    return (mute ? "Mute " : "Unmute ") + trackName (song, trackId);
}

std::string soloLabel (const Song& song, const Id& trackId, bool solo)
{
    return (solo ? "Solo " : "Unsolo ") + trackName (song, trackId);
}

bool differsOnlyInMix (const Song& a, const Song& b)
{
    if (a.tracks.size() != b.tracks.size())
        return false;
    return withMixOf (a, b) == b;
}
} // namespace overdub::song
