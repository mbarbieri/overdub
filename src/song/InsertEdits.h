#pragma once

#include "core/Error.h"
#include "song/Song.h"

#include <cstddef>
#include <string>

// Insert Chain operations (issue #33) as pure functions. Each returns the changed Song; the caller
// makes it one undo step with its label (SongPackage::edit). An empty trackId names the Master's
// chain. Reordering and bypass come with the chain editor (issue #34); a bypassed slot keeps its
// place, plugin and state, and the engine plays the chain as if it were not there.
// Errors: song.track_not_found (track_id), song.insert_not_found (slot_id).
namespace overdub::song
{
// The Song with `slot` appended to the end of that chain.
core::Expected<Song> addInsert (Song song, const Id& trackId, InsertSlot slot);
// The Song without the slot, from whichever chain holds it.
core::Expected<Song> removeInsert (Song song, const Id& slotId);
// The Song with the slot moved to `toIndex` of its own chain (clamped to the chain's last place);
// the other slots keep their order.
core::Expected<Song> moveInsert (Song song, const Id& slotId, std::size_t toIndex);
// The Song with the slot bypassed (or not).
core::Expected<Song> setInsertBypassed (Song song, const Id& slotId, bool bypassed);

// "Add AUDelay to Guitar" / "Add AUDelay to Master". An unknown Track is named "Track".
std::string addInsertLabel (const Song& song, const Id& trackId, const std::string& pluginName);
// "Remove AUDelay from Guitar" / "Remove AUDelay from Master"; "Remove Insert" for an unknown slot.
std::string removeInsertLabel (const Song& song, const Id& slotId);
// "Move AUDelay in Guitar" / "Move AUDelay in Master"; "Move Insert" for an unknown slot.
std::string moveInsertLabel (const Song& song, const Id& slotId);
// "Bypass AUDelay on Guitar" / "Unbypass AUDelay on Master"; "Bypass Insert" / "Unbypass Insert"
// for an unknown slot.
std::string bypassInsertLabel (const Song& song, const Id& slotId, bool bypassed);
} // namespace overdub::song
