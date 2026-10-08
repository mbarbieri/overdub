#include "song/InsertEdits.h"

#include "song/MixEdits.h"

#include <algorithm>

namespace overdub::song
{
namespace
{
core::Error insertNotFound (const Id& slotId)
{
    return core::Error { "song.insert_not_found", "no Insert Chain holds this slot", { { "slot_id", slotId } } };
}

std::string chainOwnerName (const Song& song, const Id& trackId)
{
    if (trackId.empty())
        return "Master";
    const auto track = std::ranges::find (song.tracks, trackId, &Track::id);
    return track != song.tracks.end() ? track->name : "Track";
}

// Where a slot is: the chain's owner (empty Track id = the Master) and the slot itself.
struct SlotPlace
{
    Id trackId;
    const InsertSlot* slot = nullptr;
};

SlotPlace findSlot (const Song& song, const Id& slotId)
{
    for (const auto& track : song.tracks)
        if (const auto found = std::ranges::find (track.insertChain, slotId, &InsertSlot::id); found != track.insertChain.end())
            return { track.id, &*found };
    if (const auto found = std::ranges::find (song.master.insertChain, slotId, &InsertSlot::id); found != song.master.insertChain.end())
        return { {}, &*found };
    return {};
}
} // namespace

core::Expected<Song> addInsert (Song song, const Id& trackId, InsertSlot slot)
{
    if (trackId.empty())
    {
        song.master.insertChain.push_back (std::move (slot));
        return song;
    }
    const auto track = std::ranges::find (song.tracks, trackId, &Track::id);
    if (track == song.tracks.end())
        return std::unexpected (trackNotFound (trackId));
    track->insertChain.push_back (std::move (slot));
    return song;
}

core::Expected<Song> removeInsert (Song song, const Id& slotId)
{
    const auto removeFrom = [&slotId] (std::vector<InsertSlot>& chain) { return std::erase_if (chain, [&slotId] (const InsertSlot& slot) { return slot.id == slotId; }) > 0; };
    for (auto& track : song.tracks)
        if (removeFrom (track.insertChain))
            return song;
    if (removeFrom (song.master.insertChain))
        return song;
    return std::unexpected (insertNotFound (slotId));
}

std::string addInsertLabel (const Song& song, const Id& trackId, const std::string& pluginName)
{
    return "Add " + pluginName + " to " + chainOwnerName (song, trackId);
}

std::string removeInsertLabel (const Song& song, const Id& slotId)
{
    const auto place = findSlot (song, slotId);
    if (place.slot == nullptr)
        return "Remove Insert";
    return "Remove " + place.slot->name + " from " + chainOwnerName (song, place.trackId);
}
} // namespace overdub::song
