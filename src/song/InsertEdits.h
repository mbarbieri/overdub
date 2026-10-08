#pragma once

#include "core/Error.h"
#include "song/Song.h"

#include <string>

// Insert Chain operations (issue #33) as pure functions. Each returns the changed Song; the caller
// makes it one undo step with its label (SongPackage::edit). An empty trackId names the Master's
// chain. Reordering and bypass come with the chain editor (slice 13).
// Errors: song.track_not_found (track_id), song.insert_not_found (slot_id).
namespace overdub::song
{
// The Song with `slot` appended to the end of that chain.
core::Expected<Song> addInsert (Song song, const Id& trackId, InsertSlot slot);
// The Song without the slot, from whichever chain holds it.
core::Expected<Song> removeInsert (Song song, const Id& slotId);

// "Add AUDelay to Guitar" / "Add AUDelay to Master". An unknown Track is named "Track".
std::string addInsertLabel (const Song& song, const Id& trackId, const std::string& pluginName);
// "Remove AUDelay from Guitar" / "Remove AUDelay from Master"; "Remove Insert" for an unknown slot.
std::string removeInsertLabel (const Song& song, const Id& slotId);
} // namespace overdub::song
