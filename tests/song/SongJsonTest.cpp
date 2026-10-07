#include "song/SongJson.h"

#include "FullSong.h"

#include <catch2/catch_test_macros.hpp>

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
    CHECK (*restored == original);
}

TEST_CASE ("the fully populated test Song sets every field away from its default", "[song][json]")
{
    // Guards the round-trip test: a field left at its default would round-trip trivially.
    const auto document = songToJson (overdub::song::testing::fullyPopulatedSong());
    const auto defaults = songToJson (overdub::song::makeEmptySong (48000));

    for (const auto& [key, value] : defaults.items())
        if (key != "formatVersion")
            CHECK (document.at (key) != value);
}

TEST_CASE ("song.json carries formatVersion 1 and the key names fixed by issues #8 and #14", "[song][json]")
{
    const auto document = songToJson (overdub::song::testing::fullyPopulatedSong());

    CHECK (document.at ("formatVersion") == 1);
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
    CHECK (result.error().fields.at ("path") == "$.tracks[1].clips[0].gainDb");
}

TEST_CASE ("a missing field is an Error naming the JSON path", "[song][json]")
{
    auto document = songToJson (overdub::song::testing::fullyPopulatedSong());
    document["takes"][0].erase ("origin");

    const auto result = songFromJson (document);

    REQUIRE_FALSE (result.has_value());
    CHECK (result.error().code == "song.json_invalid");
    CHECK (result.error().fields.at ("path") == "$.takes[0].origin");
}

TEST_CASE ("an unknown Clip source kind is an Error naming the JSON path", "[song][json]")
{
    auto document = songToJson (overdub::song::testing::fullyPopulatedSong());
    document["tracks"][0]["clips"][0]["source"]["kind"] = "midi";

    const auto result = songFromJson (document);

    REQUIRE_FALSE (result.has_value());
    CHECK (result.error().fields.at ("path") == "$.tracks[0].clips[0].source.kind");
}

TEST_CASE ("text that is not JSON is an Error, not an exception", "[song][json]")
{
    const auto result = overdub::song::parseJsonText ("{ \"formatVersion\": 1, ");

    REQUIRE_FALSE (result.has_value());
    CHECK (result.error().code == "song.json_unparsable");
}
