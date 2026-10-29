#pragma once

#include "core/Error.h"
#include "song/Song.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

// Adding an empty Track to record on (issue #109), deleting one (issue #124) and moving one
// (issue #138) as pure
// functions. The caller makes the returned Song one undo step with the returned label
// (SongPackage::edit). Armed and the input channels stay outside history
// (keepStateOutsideHistory), so undo removes an added Track and redo brings it back Armed as it
// was added.
namespace overdub::song
{
struct AddedTrack
{
    Song song;
    Id trackId;
    std::string label; // "Add Track 2"
};

// "Track N" with the lowest N >= 1 that no Track in the Song is named.
std::string nextTrackName (const Song& song);

// The Song with one new empty Track appended (no Clips, default mix, empty Insert Chain), named
// nextTrackName, coloured newTrackColour, Armed and recording `inputChannels` (empty = no input).
AddedTrack addTrack (Song song, std::vector<int> inputChannels, const PickIndex& pickColour);

struct DeletedTrack
{
    Song song;
    std::string label; // "Delete Track Guitar"
};

// The Song without the Track: its Clips, mix, Transcribe Mix and Insert Chain go with it, and it is
// no longer the Focus Track (issue #226). The Song's Takes and
// Backing Tracks stay (media is never deleted), so a deleted Track's Takes dangle (Take::trackId).
// Errors: song.track_not_found (track_id).
core::Expected<DeletedTrack> deleteTrack (Song song, const Id& trackId);

inline constexpr auto moveTrackLabel = "Move Track";

struct MovedTrack
{
    Song song;
    std::string label; // moveTrackLabel
};

// The Song with the Track moved to `toIndex` of the Track order (clamped to the last place); the
// other Tracks keep their order and nothing else changes. No MovedTrack (no undo step) when the
// Track is already at that index.
// Errors: song.track_not_found (track_id).
core::Expected<std::optional<MovedTrack>> moveTrack (Song song, const Id& trackId, std::size_t toIndex);
} // namespace overdub::song
