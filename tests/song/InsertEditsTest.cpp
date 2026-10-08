#include "song/InsertEdits.h"

#include <catch2/catch_test_macros.hpp>

using overdub::song::addInsert;
using overdub::song::addInsertLabel;
using overdub::song::InsertSlot;
using overdub::song::removeInsert;
using overdub::song::removeInsertLabel;
using overdub::song::Song;

namespace
{
Song songWithGuitar()
{
    auto song = overdub::song::makeEmptySong (48000);
    overdub::song::Track guitar;
    guitar.id = "track-guitar";
    guitar.name = "Guitar";
    song.tracks.push_back (guitar);
    return song;
}

InsertSlot slotNamed (const std::string& id, const std::string& name)
{
    return InsertSlot { .id = id, .component = { "aufx", "dely", "appl" }, .name = name };
}
} // namespace

TEST_CASE ("an Insert is added to the end of a Track's or the Master's chain", "[song][inserts]")
{
    auto song = addInsert (songWithGuitar(), "track-guitar", slotNamed ("slot-1", "AUDelay"));
    REQUIRE (song.has_value());
    song = addInsert (*song, "track-guitar", slotNamed ("slot-2", "AUPeakLimiter"));
    REQUIRE (song.has_value());
    song = addInsert (*song, "", slotNamed ("slot-3", "AUDelay"));
    REQUIRE (song.has_value());

    REQUIRE (song->tracks[0].insertChain.size() == 2);
    CHECK (song->tracks[0].insertChain[0].id == "slot-1");
    CHECK (song->tracks[0].insertChain[1].id == "slot-2");
    REQUIRE (song->master.insertChain.size() == 1);
    CHECK (song->master.insertChain[0].id == "slot-3");

    const auto unknown = addInsert (songWithGuitar(), "track-nope", slotNamed ("slot-1", "AUDelay"));
    REQUIRE_FALSE (unknown.has_value());
    CHECK (unknown.error().code == "song.track_not_found");
}

TEST_CASE ("an Insert is removed from whichever chain holds it", "[song][inserts]")
{
    auto song = *addInsert (songWithGuitar(), "track-guitar", slotNamed ("slot-1", "AUDelay"));
    song = *addInsert (song, "", slotNamed ("slot-2", "AUDelay"));

    const auto withoutTrackSlot = removeInsert (song, "slot-1");
    REQUIRE (withoutTrackSlot.has_value());
    CHECK (withoutTrackSlot->tracks[0].insertChain.empty());
    CHECK (withoutTrackSlot->master.insertChain.size() == 1);

    const auto withoutMasterSlot = removeInsert (song, "slot-2");
    REQUIRE (withoutMasterSlot.has_value());
    CHECK (withoutMasterSlot->master.insertChain.empty());

    const auto unknown = removeInsert (song, "slot-nope");
    REQUIRE_FALSE (unknown.has_value());
    CHECK (unknown.error().code == "song.insert_not_found");
    CHECK (unknown.error().fields.at ("slot_id") == "slot-nope");
}

TEST_CASE ("Insert undo labels name the plugin and the chain's owner", "[song][inserts]")
{
    auto song = *addInsert (songWithGuitar(), "track-guitar", slotNamed ("slot-1", "AUDelay"));
    song = *addInsert (song, "", slotNamed ("slot-2", "AUPeakLimiter"));

    CHECK (addInsertLabel (song, "track-guitar", "AUDelay") == "Add AUDelay to Guitar");
    CHECK (addInsertLabel (song, "", "AUDelay") == "Add AUDelay to Master");
    CHECK (removeInsertLabel (song, "slot-1") == "Remove AUDelay from Guitar");
    CHECK (removeInsertLabel (song, "slot-2") == "Remove AUPeakLimiter from Master");
    CHECK (removeInsertLabel (song, "slot-nope") == "Remove Insert");
}
