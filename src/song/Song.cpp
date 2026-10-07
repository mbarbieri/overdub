#include "song/Song.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <map>

namespace overdub::song
{
Id makeId()
{
    return juce::Uuid().toDashedString().toLowerCase().toStdString();
}

Song makeEmptySong (int sampleRate)
{
    Song song;
    song.sampleRate = sampleRate;
    return song;
}

namespace
{
// Every Insert Chain slot of the Song, Master's included, as slot id to state hash.
std::map<Id, std::string> stateHashBySlotId (const Song& song)
{
    std::map<Id, std::string> hashes;
    for (const auto& slot : song.master.insertChain)
        hashes[slot.id] = slot.stateHash;
    for (const auto& track : song.tracks)
        for (const auto& slot : track.insertChain)
            hashes[slot.id] = slot.stateHash;
    return hashes;
}

void keepCurrentStateHashes (std::vector<InsertSlot>& insertChain, const std::map<Id, std::string>& currentHashes)
{
    for (auto& slot : insertChain)
        if (const auto current = currentHashes.find (slot.id); current != currentHashes.end())
            slot.stateHash = current->second;
}
} // namespace

Song keepStateOutsideHistory (Song snapshot, const Song& current)
{
    snapshot.preRoll = current.preRoll;
    snapshot.cameraArmed = current.cameraArmed;
    snapshot.cameraDeviceId = current.cameraDeviceId;
    snapshot.playhead = current.playhead;

    const bool exportChoiceExists = ! current.exportVideoTakeId.has_value()
                                    || std::ranges::any_of (snapshot.videoTakes, [&] (const VideoTake& videoTake)
                                                            { return videoTake.id == *current.exportVideoTakeId; });
    if (exportChoiceExists)
        snapshot.exportVideoTakeId = current.exportVideoTakeId;

    for (auto& track : snapshot.tracks)
    {
        const auto currentTrack = std::ranges::find (current.tracks, track.id, &Track::id);
        if (currentTrack == current.tracks.end())
            continue;
        track.armed = currentTrack->armed;
        track.inputChannels = currentTrack->inputChannels;
    }

    const auto currentHashes = stateHashBySlotId (current);
    keepCurrentStateHashes (snapshot.master.insertChain, currentHashes);
    for (auto& track : snapshot.tracks)
        keepCurrentStateHashes (track.insertChain, currentHashes);
    return snapshot;
}

namespace
{
void addHashes (const std::vector<InsertSlot>& insertChain, std::set<std::string>& hashes)
{
    for (const auto& slot : insertChain)
        if (! slot.stateHash.empty())
            hashes.insert (slot.stateHash);
}
} // namespace

std::set<std::string> pluginStateHashes (const Song& song)
{
    std::set<std::string> hashes;
    addHashes (song.master.insertChain, hashes);
    for (const auto& track : song.tracks)
        addHashes (track.insertChain, hashes);
    return hashes;
}
} // namespace overdub::song
