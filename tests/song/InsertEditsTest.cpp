#include "song/InsertEdits.h"

#include "song/SongPackage.h"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <vector>

using overdub::song::addInsert;
using overdub::song::addInsertLabel;
using overdub::song::bypassInsertLabel;
using overdub::song::InsertSlot;
using overdub::song::moveInsert;
using overdub::song::moveInsertLabel;
using overdub::song::removeInsert;
using overdub::song::removeInsertLabel;
using overdub::song::setInsertBypassed;
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

// Reorder and bypass (issue #34).
namespace
{
Song guitarWithThreeInserts()
{
    auto song = *addInsert (songWithGuitar(), "track-guitar", slotNamed ("slot-1", "AUDelay"));
    song = *addInsert (song, "track-guitar", slotNamed ("slot-2", "AUPeakLimiter"));
    song = *addInsert (song, "track-guitar", slotNamed ("slot-3", "AUReverb"));
    return *addInsert (song, "", slotNamed ("slot-m", "AUPeakLimiter"));
}

std::vector<std::string> chainIds (const std::vector<InsertSlot>& chain)
{
    std::vector<std::string> ids;
    for (const auto& slot : chain)
        ids.push_back (slot.id);
    return ids;
}

// A package in a temp folder that deletes itself.
struct TempPackage
{
    juce::File folder = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("overdub-insert-edits-" + juce::Uuid().toString());
    std::unique_ptr<overdub::song::SongPackage> package;

    explicit TempPackage (Song song)
    {
        auto created = overdub::song::SongPackage::create (folder.getChildFile ("Song.overdub"), std::move (song));
        REQUIRE (created.has_value());
        package = std::move (*created);
    }

    ~TempPackage()
    {
        package.reset();
        folder.deleteRecursively();
    }
};
} // namespace

TEST_CASE ("an Insert moves to a new place in its own chain", "[song][inserts]")
{
    const auto song = guitarWithThreeInserts();

    CHECK (chainIds (moveInsert (song, "slot-3", 0)->tracks[0].insertChain) == std::vector<std::string> { "slot-3", "slot-1", "slot-2" });
    CHECK (chainIds (moveInsert (song, "slot-1", 1)->tracks[0].insertChain) == std::vector<std::string> { "slot-2", "slot-1", "slot-3" });
    // Past the end lands last; the other chain is untouched.
    const auto moved = moveInsert (song, "slot-1", 99);
    CHECK (chainIds (moved->tracks[0].insertChain) == std::vector<std::string> { "slot-2", "slot-3", "slot-1" });
    CHECK (moved->master == song.master);
    CHECK (*moveInsert (song, "slot-2", 1) == song);
    CHECK (*moveInsert (song, "slot-m", 0) == song);

    const auto unknown = moveInsert (song, "slot-nope", 0);
    REQUIRE_FALSE (unknown.has_value());
    CHECK (unknown.error().code == "song.insert_not_found");
}

TEST_CASE ("an Insert is bypassed and back on, keeping its place and state", "[song][inserts]")
{
    auto song = guitarWithThreeInserts();
    song.settings.pluginStateHashBySlot["slot-2"] = "abc";

    const auto bypassed = setInsertBypassed (song, "slot-2", true);
    REQUIRE (bypassed.has_value());
    CHECK (bypassed->tracks[0].insertChain[1].bypassed);
    CHECK (pluginStateHash (*bypassed, "slot-2") == "abc");
    CHECK (chainIds (bypassed->tracks[0].insertChain) == chainIds (song.tracks[0].insertChain));
    CHECK_FALSE (bypassed->tracks[0].insertChain[0].bypassed);
    CHECK (setInsertBypassed (song, "slot-m", true)->master.insertChain[0].bypassed);
    CHECK (*setInsertBypassed (*bypassed, "slot-2", false) == song);

    const auto unknown = setInsertBypassed (song, "slot-nope", true);
    REQUIRE_FALSE (unknown.has_value());
    CHECK (unknown.error().code == "song.insert_not_found");
}

TEST_CASE ("move and bypass undo labels name the plugin and the chain's owner", "[song][inserts]")
{
    const auto song = guitarWithThreeInserts();
    CHECK (moveInsertLabel (song, "slot-1") == "Move AUDelay in Guitar");
    CHECK (moveInsertLabel (song, "slot-m") == "Move AUPeakLimiter in Master");
    CHECK (moveInsertLabel (song, "slot-nope") == "Move Insert");
    CHECK (bypassInsertLabel (song, "slot-2", true) == "Bypass AUPeakLimiter on Guitar");
    CHECK (bypassInsertLabel (song, "slot-m", false) == "Unbypass AUPeakLimiter on Master");
    CHECK (bypassInsertLabel (song, "slot-nope", true) == "Bypass Insert");
}

TEST_CASE ("reorder, bypass and remove are each one undo step that undo reverts", "[song][inserts][history]")
{
    const auto original = guitarWithThreeInserts();
    TempPackage temp { original };
    auto& package = *temp.package;

    REQUIRE (package.edit (*moveInsert (package.song(), "slot-3", 0), moveInsertLabel (package.song(), "slot-3")).has_value());
    const auto afterMove = package.song();
    REQUIRE (package.edit (*setInsertBypassed (package.song(), "slot-1", true), bypassInsertLabel (package.song(), "slot-1", true)).has_value());
    const auto afterBypass = package.song();
    REQUIRE (package.edit (*removeInsert (package.song(), "slot-2"), removeInsertLabel (package.song(), "slot-2")).has_value());
    CHECK (chainIds (package.song().tracks[0].insertChain) == std::vector<std::string> { "slot-3", "slot-1" });

    CHECK (package.undoLabel() == "Remove AUPeakLimiter from Guitar");
    REQUIRE (package.undo().has_value());
    CHECK (package.song().tracks[0].insertChain == afterBypass.tracks[0].insertChain);
    CHECK (package.undoLabel() == "Bypass AUDelay on Guitar");
    REQUIRE (package.undo().has_value());
    CHECK (package.song().tracks[0].insertChain == afterMove.tracks[0].insertChain);
    CHECK (package.undoLabel() == "Move AUReverb in Guitar");
    REQUIRE (package.undo().has_value());
    CHECK (package.song().tracks[0].insertChain == original.tracks[0].insertChain);

    REQUIRE (package.redo().has_value());
    CHECK (package.song().tracks[0].insertChain == afterMove.tracks[0].insertChain);
}
