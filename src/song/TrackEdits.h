#pragma once

#include "song/Song.h"

#include <string>
#include <vector>

// Adding an empty Track to record on (issue #109) as a pure function. The caller makes the
// returned Song one undo step with the returned label (SongPackage::edit). Armed and the input
// channels stay outside history (keepStateOutsideHistory), so undo removes the Track and redo
// brings it back Armed as it was added.
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
// nextTrackName, Armed and recording `inputChannels` (empty = no input).
AddedTrack addTrack (Song song, std::vector<int> inputChannels);
} // namespace overdub::song
