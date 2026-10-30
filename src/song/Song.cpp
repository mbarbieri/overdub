#include "song/Song.h"

#include "song/SampleRateConversion.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <map>
#include <utility>

namespace overdub::song
{
Id makeId()
{
    return juce::Uuid().toDashedString().toLowerCase().toStdString();
}

const Track* findTrack (const Song& song, const Id& trackId)
{
    const auto track = std::ranges::find (song.tracks, trackId, &Track::id);
    return track != song.tracks.end() ? &*track : nullptr;
}

Track* findTrack (Song& song, const Id& trackId)
{
    return const_cast<Track*> (findTrack (std::as_const (song), trackId));
}

core::Expected<const Track*> lookupTrack (const Song& song, const Id& trackId)
{
    if (const auto* track = findTrack (song, trackId))
        return track;
    return std::unexpected (core::Error { "song.track_not_found", "no Track with this id in the Song", { { "track_id", trackId } } });
}

core::Expected<Track*> lookupTrack (Song& song, const Id& trackId)
{
    const auto track = lookupTrack (std::as_const (song), trackId);
    if (! track.has_value())
        return std::unexpected (track.error());
    return const_cast<Track*> (*track);
}

std::string_view toString (ClipSourceKind kind)
{
    switch (kind)
    {
        case ClipSourceKind::take:
            return "take";
        case ClipSourceKind::backing:
            return "backing";
    }
    return "take";
}

int preRollSeconds (PreRoll preRoll)
{
    switch (preRoll)
    {
        case PreRoll::off: return 0;
        case PreRoll::oneSecond: return 1;
        case PreRoll::twoSeconds: return 2;
        case PreRoll::fourSeconds: return 4;
    }
    return 0;
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

// The current settings as a restored snapshot keeps them (see keepStateOutsideHistory).
SongSettings keptSettings (const Song& snapshot, const Song& current)
{
    auto settings = current.settings;
    // A step across a sample rate conversion keeps the playhead at the same time.
    if (snapshot.sampleRate != current.sampleRate)
        settings.playhead = convertedSamples (current.settings.playhead, current.sampleRate, snapshot.sampleRate);
    const auto hasLoop = [&] (const std::optional<Id>& loopId) { return loopId.has_value() && std::ranges::contains (snapshot.loops, *loopId, &Loop::id); };
    if (settings.exportVideoTakeId.has_value() && ! std::ranges::contains (snapshot.videoTakes, *settings.exportVideoTakeId, &VideoTake::id))
        settings.exportVideoTakeId = snapshot.settings.exportVideoTakeId;
    if (! hasLoop (settings.exportLoopId))
        settings.exportLoopId.reset();
    if (settings.focusTrackId.has_value() && findTrack (snapshot, *settings.focusTrackId) == nullptr)
        settings.focusTrackId.reset();
    if (! hasLoop (settings.activeLoopId))
    {
        settings.activeLoopId.reset();
        settings.looping = false;
    }
    return settings;
}
} // namespace

Song keepStateOutsideHistory (Song snapshot, const Song& current)
{
    snapshot.settings = keptSettings (snapshot, current);
    for (auto& track : snapshot.tracks)
        if (const auto* currentTrack = findTrack (current, track.id); currentTrack != nullptr)
            track.settings = currentTrack->settings;
    for (auto& loop : snapshot.loops)
        if (const auto found = std::ranges::find (current.loops, loop.id, &Loop::id); found != current.loops.end())
            loop.settings = found->settings;

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

std::vector<std::string> trackColours (const Song& song)
{
    std::vector<std::string> colours;
    for (const auto& track : song.tracks)
        colours.push_back (track.colour);
    return colours;
}

std::set<std::string> pluginStateHashes (const Song& song)
{
    std::set<std::string> hashes;
    addHashes (song.master.insertChain, hashes);
    for (const auto& track : song.tracks)
        addHashes (track.insertChain, hashes);
    return hashes;
}
} // namespace overdub::song
