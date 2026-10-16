#include "song/Song.h"

#include <juce_core/juce_core.h>

#include <algorithm>

namespace overdub::song
{
Id makeId()
{
    return juce::Uuid().toDashedString().toLowerCase().toStdString();
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

const TrackInput& trackInput (const Song& song, const Id& trackId)
{
    static const TrackInput none;
    const auto found = song.settings.trackInputs.find (trackId);
    return found != song.settings.trackInputs.end() ? found->second : none;
}

const std::string& pluginStateHash (const Song& song, const Id& slotId)
{
    static const std::string none;
    const auto found = song.settings.pluginStateHashBySlot.find (slotId);
    return found != song.settings.pluginStateHashBySlot.end() ? found->second : none;
}

namespace
{
std::set<Id> trackIds (const Song& song)
{
    std::set<Id> ids;
    for (const auto& track : song.tracks)
        ids.insert (track.id);
    return ids;
}

// Every Insert Chain slot id of the Song, Master's included.
std::set<Id> insertSlotIds (const Song& song)
{
    std::set<Id> ids;
    for (const auto& slot : song.master.insertChain)
        ids.insert (slot.id);
    for (const auto& track : song.tracks)
        for (const auto& slot : track.insertChain)
            ids.insert (slot.id);
    return ids;
}

// An entry for each id the snapshot has: the current one when the current Song has that id too,
// else the snapshot's.
template <typename Entry>
std::map<Id, Entry> entriesAfterRestore (const std::set<Id>& snapshotIds,
                                         const std::map<Id, Entry>& snapshotEntries,
                                         const std::set<Id>& currentIds,
                                         const std::map<Id, Entry>& currentEntries)
{
    std::map<Id, Entry> entries;
    for (const auto& id : snapshotIds)
    {
        const auto& from = currentIds.contains (id) ? currentEntries : snapshotEntries;
        if (const auto entry = from.find (id); entry != from.end())
            entries.emplace (id, entry->second);
    }
    return entries;
}
} // namespace

Song withContentOf (Song snapshot, const Song& current)
{
    auto settings = current.settings;
    settings.trackInputs = entriesAfterRestore (trackIds (snapshot), snapshot.settings.trackInputs, trackIds (current), current.settings.trackInputs);
    settings.pluginStateHashBySlot = entriesAfterRestore (insertSlotIds (snapshot), snapshot.settings.pluginStateHashBySlot, insertSlotIds (current), current.settings.pluginStateHashBySlot);

    const auto& exportChoice = current.settings.exportVideoTakeId;
    if (exportChoice.has_value() && ! std::ranges::contains (snapshot.videoTakes, *exportChoice, &VideoTake::id))
        settings.exportVideoTakeId = snapshot.settings.exportVideoTakeId;

    snapshot.settings = std::move (settings);
    return snapshot;
}

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
    for (const auto& slotId : insertSlotIds (song))
        if (const auto& hash = pluginStateHash (song, slotId); ! hash.empty())
            hashes.insert (hash);
    return hashes;
}
} // namespace overdub::song
