#include "song/TranscribeMix.h"

#include <string>

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

core::Expected<Song> setOctaveShift (Song song, const Id& trackId, int octaves)
{
    if (octaves < minOctaveShift || octaves > maxOctaveShift)
        return std::unexpected (core::Error {
            "song.octave_shift_out_of_range",
            "the octave shift is a whole number of octaves from -1 to +2",
            { { "octaves", octaves }, { "min", minOctaveShift }, { "max", maxOctaveShift } },
        });
    auto track = lookupTrack (song, trackId);
    if (! track.has_value())
        return std::unexpected (track.error());
    (*track)->octaveShift = octaves;
    return song;
}

core::Expected<Song> setBandFilter (Song song, const Id& trackId, BandFilter filter)
{
    const auto inRange = [] (int hz) { return hz >= minBandFilterHz && hz <= maxBandFilterHz; };
    if (! inRange (filter.lowHz) || ! inRange (filter.highHz))
        return std::unexpected (core::Error {
            "song.band_filter_out_of_range",
            "the band filter's cuts are whole Hz from " + std::to_string (minBandFilterHz) + " to " + std::to_string (maxBandFilterHz),
            { { "low_hz", filter.lowHz }, { "high_hz", filter.highHz }, { "min_hz", minBandFilterHz }, { "max_hz", maxBandFilterHz } },
        });
    if (filter.lowHz >= filter.highHz)
        return std::unexpected (core::Error {
            "song.band_filter_low_not_below_high",
            "the band filter's low cut must be below its high cut",
            { { "low_hz", filter.lowHz }, { "high_hz", filter.highHz } },
        });
    auto track = lookupTrack (song, trackId);
    if (! track.has_value())
        return std::unexpected (track.error());
    (*track)->bandFilter = filter;
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

int focusOctaveShift (const Song& song)
{
    const auto* track = focusTrack (song);
    return track != nullptr ? track->octaveShift : 0;
}
} // namespace overdub::song
