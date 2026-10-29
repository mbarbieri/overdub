#include "song/SongJson.h"

#include "FullSong.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

using overdub::song::Song;
using overdub::song::songFromJson;
using overdub::song::songToJson;

TEST_CASE ("a fully populated Song round-trips through song.json equal", "[song][json]")
{
    const Song original = overdub::song::testing::fullyPopulatedSong();

    // Through text, as on disk.
    const auto text = songToJson (original).dump (2);
    const auto parsed = overdub::song::parseJsonText (text);
    REQUIRE (parsed.has_value());

    const auto restored = songFromJson (*parsed);
    REQUIRE (restored.has_value());
    CHECK (*restored == original); // every field of every element, in order
    CHECK (songToJson (*restored) == songToJson (original));

    const auto clipIds = [] (const overdub::song::Track& track) {
        std::vector<std::string> ids;
        for (const auto& clip : track.clips)
            ids.push_back (clip.id);
        return ids;
    };
    CHECK (clipIds (restored->tracks.at (0)) == std::vector<std::string> { "clip-b", "clip-a" });
    CHECK (clipIds (restored->tracks.at (1)) == std::vector<std::string> { "clip-d", "clip-c" });
}

namespace
{
using nlohmann::json;
using Leaves = std::map<std::string, std::vector<json>>;

// Collects every leaf value by its path, with list indices written as "[]": the values of one
// field across every element of a list share a path. Lists of numbers count as leaves.
void collectLeaves (const json& node, const std::string& path, Leaves& leaves)
{
    if (node.is_object())
        for (const auto& [key, value] : node.items())
            collectLeaves (value, path + "." + key, leaves);
    else if (node.is_array() && ! node.empty() && node.front().is_object())
        for (const auto& element : node)
            collectLeaves (element, path + "[]", leaves);
    else
        leaves[path].push_back (node);
}

// Every list of entities (not a list of numbers such as inputChannels) holds two or more.
void checkListsHoldTwoOrMore (const json& node, const std::string& path)
{
    if (node.is_object())
        for (const auto& [key, value] : node.items())
            checkListsHoldTwoOrMore (value, path + "." + key);
    if (! node.is_array() || (! node.empty() && ! node.front().is_object()))
        return;

    INFO ("list " << path);
    CHECK (node.size() >= 2);
    for (const auto& element : node)
        checkListsHoldTwoOrMore (element, path + "[]");
}

// One default-constructed element in every list, so every field has a default to compare with.
Song songOfDefaults()
{
    using namespace overdub::song;
    Song song;
    song.tracks = { Track { .insertChain = { InsertSlot {} }, .clips = { Clip {} } } };
    song.master.insertChain = { InsertSlot {} };
    song.passes = { Pass {} };
    song.takes = { Take {} };
    song.backingTracks = { BackingTrack {} };
    song.videoTakes = { VideoTake {} };
    song.markers = { Marker {} };
    song.loops = { Loop { .end = 1 } };
    return song;
}
} // namespace

TEST_CASE ("the fully populated test Song sets every field away from its default", "[song][json]")
{
    // Guards the round-trip test: a field left at its default would round-trip trivially.
    const auto document = songToJson (overdub::song::testing::fullyPopulatedSong());
    Leaves populated, defaults;
    collectLeaves (document, "$", populated);
    collectLeaves (songToJson (songOfDefaults()), "$", defaults);

    for (const auto& [path, defaultValues] : defaults)
    {
        if (path == "$.formatVersion")
            continue;
        INFO ("field " << path);
        REQUIRE (populated.contains (path));
        const auto& values = populated.at (path);
        CHECK (std::ranges::any_of (values, [&] (const json& value) { return value != defaultValues.front(); }));
    }
    checkListsHoldTwoOrMore (document, "$");
}

TEST_CASE ("song.json carries formatVersion 9 and the key names fixed by issues #8, #14, #68, #114, #137, #201, #209, #226, #227 and #228", "[song][json]")
{
    const auto document = songToJson (overdub::song::testing::fullyPopulatedSong());

    CHECK (document.at ("formatVersion") == 9);
    CHECK (document.at ("loops").at (0) == nlohmann::json { { "id", "loop-1" }, { "name", "Intro riff" }, { "start", 24000 }, { "end", 72000 } });
    CHECK (document.at ("loops").size() == 2);
    CHECK (document.at ("activeLoopId") == "loop-2");
    CHECK (document.at ("looping") == true);
    CHECK (songToJson (overdub::song::Song {}).at ("loops") == nlohmann::json::array());
    CHECK (songToJson (overdub::song::Song {}).at ("activeLoopId").is_null());
    CHECK (songToJson (overdub::song::Song {}).at ("looping") == false);
    CHECK (document.at ("markers") == nlohmann::json::array ({ { { "id", "marker-1" }, { "name", "Verse" }, { "position", 48000 } }, { { "id", "marker-2" }, { "name", "Chorus" }, { "position", 96000 } } }));
    CHECK (songToJson (overdub::song::Song {}).at ("markers") == nlohmann::json::array());
    CHECK (document.at ("focusTrackId") == "track-backing");
    CHECK (songToJson (overdub::song::Song {}).at ("focusTrackId").is_null());
    CHECK (document.at ("tracks").at (0).at ("transcribeMute") == true);
    CHECK (document.at ("tracks").at (0).at ("transcribeSolo") == false);
    CHECK (document.at ("tracks").at (1).at ("transcribeMute") == false);
    CHECK (document.at ("tracks").at (1).at ("transcribeSolo") == true);
    CHECK (document.at ("transpose") == -3);
    CHECK (songToJson (overdub::song::Song {}).at ("transpose") == 0);
    CHECK (document.at ("exportFormat") == "mp4");
    CHECK (document.at ("exportLength") == "full_song");
    CHECK (document.at ("exportDestination") == "/Users/someone/Music/Mixes/Band Mix.mp4");
    CHECK (songToJson (overdub::song::Song {}).at ("exportFormat") == "wav");
    CHECK (songToJson (overdub::song::Song {}).at ("exportLength") == "trim_to_video_take");
    CHECK (document.at ("tracks").at (0).at ("colour") == "#c0ffee");
    CHECK (document.at ("tracks").at (1).at ("colour") == "#f5e19c");
    CHECK (document.at ("backingTracks").at (0).at ("separatedFrom") == nlohmann::json { { "backingTrackId", "" }, { "model", "" } });
    CHECK (document.at ("backingTracks").at (1).at ("separatedFrom") == nlohmann::json { { "backingTrackId", "backing-1" }, { "model", "bs_roformer_sw.fp32" } });
    CHECK (document.at ("videoTakes").at (0).at ("outputDevice") == "MacBook Pro Speakers");
    CHECK (document.at ("takes").at (0).contains ("trackId"));
    CHECK (document.at ("takes").at (0).contains ("passId"));
    CHECK (document.at ("tracks").at (0).at ("clips").at (0).at ("source").at ("kind") == "take");
    CHECK (document.at ("tracks").at (1).at ("clips").at (0).at ("source").at ("kind") == "backing");

    const auto& backing = document.at ("backingTracks").at (0);
    for (const char* key : { "id", "originalFilename", "originalExtension", "lengthSamples" })
        CHECK (backing.contains (key));
}

TEST_CASE ("a malformed song.json is an Error naming the JSON path", "[song][json]")
{
    auto document = songToJson (overdub::song::testing::fullyPopulatedSong());
    document["tracks"][1]["clips"][0]["gainDb"] = "loud";

    const auto result = songFromJson (document);

    REQUIRE_FALSE (result.has_value());
    CHECK (result.error().code == "song.json_invalid");
    CHECK (result.error().fields.at ("json_path") == "$.tracks[1].clips[0].gainDb");
}

TEST_CASE ("a Track colour round-trips as the stored value, palette colour or not", "[song][json][colour]")
{
    auto song = overdub::song::testing::fullyPopulatedSong();
    song.tracks.at (0).colour = "#C0FFEE";
    song.tracks.at (1).colour = "#a8d8b9";

    const auto read = songFromJson (songToJson (song));

    REQUIRE (read.has_value());
    CHECK (read->tracks.at (0).colour == "#C0FFEE");
    CHECK (read->tracks.at (1).colour == "#a8d8b9");
}

TEST_CASE ("a Track colour that is not \"#\" and six hex digits is an Error naming the JSON path", "[song][json][colour]")
{
    auto document = songToJson (overdub::song::testing::fullyPopulatedSong());
    document["tracks"][1]["colour"] = "mint";

    const auto result = songFromJson (document);

    REQUIRE_FALSE (result.has_value());
    CHECK (result.error().code == "song.json_invalid");
    CHECK (result.error().fields.at ("json_path") == "$.tracks[1].colour");
}

TEST_CASE ("a missing field is an Error naming the JSON path", "[song][json]")
{
    auto document = songToJson (overdub::song::testing::fullyPopulatedSong());
    document["takes"][0].erase ("origin");

    const auto result = songFromJson (document);

    REQUIRE_FALSE (result.has_value());
    CHECK (result.error().code == "song.json_invalid");
    CHECK (result.error().fields.at ("json_path") == "$.takes[0].origin");
}

TEST_CASE ("an unknown Clip source kind is an Error naming the JSON path", "[song][json]")
{
    auto document = songToJson (overdub::song::testing::fullyPopulatedSong());
    document["tracks"][0]["clips"][0]["source"]["kind"] = "midi";

    const auto result = songFromJson (document);

    REQUIRE_FALSE (result.has_value());
    CHECK (result.error().fields.at ("json_path") == "$.tracks[0].clips[0].source.kind");
}

TEST_CASE ("an unknown export format or length is an Error naming the JSON path", "[song][json][export]")
{
    auto document = songToJson (overdub::song::testing::fullyPopulatedSong());
    document["exportFormat"] = "flac";
    CHECK (songFromJson (document).error().fields.at ("json_path") == "$.exportFormat");

    document = songToJson (overdub::song::testing::fullyPopulatedSong());
    document["exportLength"] = "trim";
    CHECK (songFromJson (document).error().fields.at ("json_path") == "$.exportLength");
}

TEST_CASE ("a document that is not an object, or holds a wrong-shaped list, is an Error naming the JSON path", "[song][json]")
{
    CHECK (songFromJson (nlohmann::json::array()).error().fields.at ("json_path") == "$");

    auto document = songToJson (overdub::song::testing::fullyPopulatedSong());
    document["tracks"][1]["inputChannels"] = { 0, "left" };
    CHECK (songFromJson (document).error().fields.at ("json_path") == "$.tracks[1].inputChannels[1]");

    document["passes"] = "none";
    const auto result = songFromJson (document);
    CHECK (result.error().fields.at ("json_path") == "$.tracks[1].inputChannels[1]"); // the first problem in reading order
}

TEST_CASE ("text that is not JSON is an Error, not an exception", "[song][json]")
{
    const auto result = overdub::song::parseJsonText ("{ \"formatVersion\": 1, ");

    REQUIRE_FALSE (result.has_value());
    CHECK (result.error().code == "song.json_unparsable");
}
