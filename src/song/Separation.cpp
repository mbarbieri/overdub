#include "song/Separation.h"

#include <algorithm>

namespace overdub::song
{
namespace
{
core::Error notSeparable (const Id& trackId, const char* reason)
{
    return core::Error { "song.track_not_separable", "the Track's Clips do not all play one Backing Track", { { "track_id", trackId }, { "reason", reason } } };
}
} // namespace

core::Expected<Id> separableBackingTrack (const Song& song, const Id& trackId)
{
    const auto track = std::ranges::find (song.tracks, trackId, &Track::id);
    if (track == song.tracks.end())
        return std::unexpected (core::Error { "song.track_not_found", "the Song has no such Track", { { "track_id", trackId } } });
    if (track->clips.empty())
        return std::unexpected (notSeparable (trackId, "no_clips"));

    const auto playsTake = [] (const Clip& clip) { return clip.source.kind != ClipSourceKind::backing; };
    if (std::ranges::any_of (track->clips, playsTake))
        return std::unexpected (notSeparable (trackId, "take_clips"));
    const auto& first = track->clips.front().source;
    const auto playsFirst = [&first] (const Clip& clip) { return clip.source == first; };
    if (! std::ranges::all_of (track->clips, playsFirst))
        return std::unexpected (notSeparable (trackId, "several_backing_tracks"));
    if (! std::ranges::contains (song.backingTracks, first.id, &BackingTrack::id))
        return std::unexpected (notSeparable (trackId, "backing_track_missing"));
    return first.id;
}

core::Expected<Song> withSeparation (Song song, const Id& sourceTrackId, const std::vector<BackingTrack>& parts, const PickIndex& pickColour)
{
    const auto source = separableBackingTrack (song, sourceTrackId);
    if (! source.has_value())
        return std::unexpected (source.error());
    if (parts.size() != separationParts.size())
        return std::unexpected (core::Error {
            "song.separation_parts_invalid",
            "a Separation needs one Backing Track per part",
            { { "expected", separationParts.size() }, { "found", parts.size() } },
        });

    const auto sourceTrack = std::ranges::find (song.tracks, sourceTrackId, &Track::id);
    sourceTrack->mute = true;
    const auto sourceClips = sourceTrack->clips;

    auto takenColours = trackColours (song);
    std::vector<Track> newTracks;
    for (std::size_t i = 0; i < parts.size(); ++i)
    {
        Track track;
        track.id = makeId();
        track.name = std::string (separationParts[i].trackName);
        track.colour = newTrackColour (takenColours, pickColour);
        takenColours.push_back (track.colour);
        for (auto clip : sourceClips)
        {
            clip.id = makeId();
            clip.source = ClipSource { ClipSourceKind::backing, parts[i].id };
            track.clips.push_back (std::move (clip));
        }
        newTracks.push_back (std::move (track));
    }

    // Recomputed: muting did not move it, but the iterator is not kept across the insert.
    const auto below = std::ranges::find (song.tracks, sourceTrackId, &Track::id) + 1;
    song.tracks.insert (below, newTracks.begin(), newTracks.end());
    song.backingTracks.insert (song.backingTracks.end(), parts.begin(), parts.end());
    return song;
}

std::string separationStepLabel (const BackingTrack& source)
{
    return "Separate " + source.originalFilename;
}
} // namespace overdub::song
