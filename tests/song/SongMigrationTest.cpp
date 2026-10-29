#include "song/SongMigration.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using nlohmann::json;
using overdub::song::migrateDocument;
using overdub::song::MigrationStep;

namespace
{
// A made-up v1 → v2 step: renames "volume" to "volumeDb".
overdub::core::Expected<json> renameVolume (json document)
{
    document["volumeDb"] = document.at ("volume");
    document.erase ("volume");
    return document;
}

overdub::core::Expected<json> failingStep (json)
{
    return std::unexpected (overdub::core::Error { "song.test_step_failed", "cannot migrate", {} });
}
} // namespace

TEST_CASE ("a current Song needs no migration in this build", "[song][migration]")
{
    const json document = { { "formatVersion", overdub::song::songFormatVersion }, { "sampleRate", 48000 } };

    const auto migrated = overdub::song::migrateSongDocument (document);

    REQUIRE (migrated.has_value());
    CHECK (migrated->fromVersion == overdub::song::songFormatVersion);
    CHECK (migrated->document == document);
}

TEST_CASE ("a v1 Song's Video Takes migrate to v2 with no output device known", "[song][migration]")
{
    const json videoTake = { { "id", "video-1" }, { "cameraId", "camera-facetime" } };
    const json document = { { "formatVersion", 1 }, { "sampleRate", 48000 }, { "videoTakes", json::array ({ videoTake, videoTake }) } };

    const auto migrated = overdub::song::migrateSongDocument (document);

    REQUIRE (migrated.has_value());
    CHECK (migrated->fromVersion == 1);
    CHECK (migrated->document.at ("formatVersion") == overdub::song::songFormatVersion);
    for (const auto& migratedTake : migrated->document.at ("videoTakes"))
    {
        CHECK (migratedTake.at ("outputDevice") == "");
        CHECK (migratedTake.at ("cameraId") == "camera-facetime");
    }
}

TEST_CASE ("a v2 Song's Backing Tracks migrate to v3 as imported, separated from nothing", "[song][migration]")
{
    const json backingTrack = { { "id", "backing-1" }, { "originalFilename", "Band Mix.mp3" } };
    const json document = { { "formatVersion", 2 }, { "sampleRate", 48000 }, { "backingTracks", json::array ({ backingTrack, backingTrack }) } };

    const auto migrated = overdub::song::migrateSongDocument (document);

    REQUIRE (migrated.has_value());
    CHECK (migrated->fromVersion == 2);
    CHECK (migrated->document.at ("formatVersion") == overdub::song::songFormatVersion);
    for (const auto& migratedBackingTrack : migrated->document.at ("backingTracks"))
    {
        CHECK (migratedBackingTrack.at ("separatedFrom") == json { { "backingTrackId", "" }, { "model", "" } });
        CHECK (migratedBackingTrack.at ("originalFilename") == "Band Mix.mp3");
    }
}

TEST_CASE ("a v3 Song's Tracks migrate to v4 coloured palette[0], palette[1], ... in Song order, wrapping after the last", "[song][migration][colour]")
{
    json tracks = json::array();
    for (int i = 0; i < 14; ++i)
        tracks.push_back ({ { "id", "track-" + std::to_string (i) }, { "name", "Track" } });
    const json document = { { "formatVersion", 3 }, { "sampleRate", 48000 }, { "tracks", tracks } };

    const auto migrated = overdub::song::migrateSongDocument (document);

    REQUIRE (migrated.has_value());
    CHECK (migrated->fromVersion == 3);
    CHECK (migrated->document.at ("formatVersion") == overdub::song::songFormatVersion);
    std::vector<std::string> colours;
    for (const auto& track : migrated->document.at ("tracks"))
        colours.push_back (track.at ("colour").get<std::string>());
    CHECK (colours
           == std::vector<std::string> { "#a8d8b9", "#f4a6a6", "#a6d4f2", "#f5e19c", "#d3b5e8", "#9fdcd4", "#f6c39a", "#a7b8e8", "#d4e8a0", "#f5b3c8", "#bcb3ef", "#eeb1e0", "#a8d8b9", "#f4a6a6" });
    CHECK (migrated->document.at ("tracks").at (13).at ("id") == "track-13");
}

TEST_CASE ("a v4 Song migrates to v5 exporting WAV, trimmed to the Video Take, into the export folder", "[song][migration][export]")
{
    const json document = { { "formatVersion", 4 }, { "sampleRate", 48000 }, { "exportVideoTakeId", "video-1" } };

    const auto migrated = overdub::song::migrateSongDocument (document);

    REQUIRE (migrated.has_value());
    CHECK (migrated->fromVersion == 4);
    CHECK (migrated->document.at ("formatVersion") == overdub::song::songFormatVersion);
    CHECK (migrated->document.at ("exportFormat") == "wav");
    CHECK (migrated->document.at ("exportLength") == "trim_to_video_take");
    CHECK (migrated->document.at ("exportDestination") == "");
    CHECK (migrated->document.at ("exportVideoTakeId") == "video-1");
}

TEST_CASE ("each step function migrates one version and the scaffold stamps the new version", "[song][migration]")
{
    const std::vector<MigrationStep> steps { renameVolume };

    const auto migrated = migrateDocument ({ { "formatVersion", 1 }, { "volume", -6.0 } }, steps);

    REQUIRE (migrated.has_value());
    CHECK (migrated->fromVersion == 1);
    CHECK (migrated->document == json { { "formatVersion", 2 }, { "volumeDb", -6.0 } });
}

TEST_CASE ("a newer formatVersion is refused with an Error that names the version found", "[song][migration]")
{
    const auto migrated = overdub::song::migrateSongDocument ({ { "formatVersion", overdub::song::songFormatVersion + 1 } });

    REQUIRE_FALSE (migrated.has_value());
    CHECK (migrated.error().code == "song.format_too_new");
    CHECK (migrated.error().fields.at ("version_found") == overdub::song::songFormatVersion + 1);
    CHECK (migrated.error().fields.at ("version_supported") == overdub::song::songFormatVersion);
}

TEST_CASE ("a document without an integer formatVersion is refused", "[song][migration]")
{
    const auto migrated = overdub::song::migrateSongDocument ({ { "formatVersion", "one" } });

    REQUIRE_FALSE (migrated.has_value());
    CHECK (migrated.error().code == "song.json_invalid");
    CHECK (migrated.error().fields.at ("json_path") == "$.formatVersion");
}

TEST_CASE ("a failing step stops migration with an Error naming the version it started from", "[song][migration]")
{
    const std::vector<MigrationStep> steps { failingStep };

    const auto migrated = migrateDocument ({ { "formatVersion", 1 } }, steps);

    REQUIRE_FALSE (migrated.has_value());
    CHECK (migrated.error().code == "song.migration_failed");
    CHECK (migrated.error().fields.at ("from_version") == 1);
    CHECK (migrated.error().fields.at ("step_error") == "song.test_step_failed");
}

TEST_CASE ("a v5 Song migrates to v6 untransposed", "[song][migration][transpose]")
{
    const json document = { { "formatVersion", 5 }, { "sampleRate", 48000 } };

    const auto migrated = overdub::song::migrateSongDocument (document);

    REQUIRE (migrated.has_value());
    CHECK (migrated->fromVersion == 5);
    CHECK (migrated->document.at ("formatVersion") == overdub::song::songFormatVersion);
    CHECK (migrated->document.at ("transpose") == 0);
}

TEST_CASE ("a v6 Song migrates to v7 with no Focus Track and every Track unmuted and unsoloed in the Transcribe Mix", "[song][migration][transcribe]")
{
    const json document = {
        { "formatVersion", 6 },
        { "sampleRate", 48000 },
        { "tracks", { { { "id", "track-1" }, { "mute", true }, { "solo", true } }, { { "id", "track-2" } } } },
    };

    const auto migrated = overdub::song::migrateSongDocument (document);

    REQUIRE (migrated.has_value());
    CHECK (migrated->fromVersion == 6);
    CHECK (migrated->document.at ("formatVersion") == overdub::song::songFormatVersion);
    CHECK (migrated->document.at ("focusTrackId").is_null());
    for (const auto& track : migrated->document.at ("tracks"))
    {
        CHECK (track.at ("transcribeMute") == false);
        CHECK (track.at ("transcribeSolo") == false);
    }
    // The Song's own mix is untouched.
    CHECK (migrated->document.at ("tracks").at (0).at ("mute") == true);
    CHECK (migrated->document.at ("tracks").at (0).at ("solo") == true);
}

TEST_CASE ("a v7 Song migrates to v8 with no Markers", "[song][migration][markers]")
{
    const json document = { { "formatVersion", 7 }, { "sampleRate", 48000 }, { "transpose", 2 } };

    const auto migrated = overdub::song::migrateSongDocument (document);

    REQUIRE (migrated.has_value());
    CHECK (migrated->fromVersion == 7);
    CHECK (migrated->document.at ("formatVersion") == overdub::song::songFormatVersion);
    CHECK (migrated->document.at ("markers") == json::array());
    CHECK (migrated->document.at ("transpose") == 2);
}

TEST_CASE ("a v8 Song migrates to v9 with no Loops, none active and not looping", "[song][migration][loops]")
{
    const json document = { { "formatVersion", 8 }, { "sampleRate", 48000 }, { "markers", json::array() } };

    const auto migrated = overdub::song::migrateSongDocument (document);

    REQUIRE (migrated.has_value());
    CHECK (migrated->fromVersion == 8);
    CHECK (migrated->document.at ("formatVersion") == overdub::song::songFormatVersion);
    CHECK (migrated->document.at ("loops") == json::array());
    CHECK (migrated->document.at ("activeLoopId").is_null());
    CHECK (migrated->document.at ("looping") == false);
    CHECK (migrated->document.at ("markers") == json::array());
}

TEST_CASE ("a v9 Song migrates to v10 with every Loop drilled at 100%", "[song][migration][loops]")
{
    const json loop = { { "id", "loop-1" }, { "name", "Solo" }, { "start", 0 }, { "end", 48000 } };
    const json document = { { "formatVersion", 9 }, { "sampleRate", 48000 }, { "loops", json::array ({ loop, loop }) }, { "activeLoopId", "loop-1" }, { "looping", true } };

    const auto migrated = overdub::song::migrateSongDocument (document);

    REQUIRE (migrated.has_value());
    CHECK (migrated->fromVersion == 9);
    CHECK (migrated->document.at ("formatVersion") == 10);
    REQUIRE (migrated->document.at ("loops").size() == 2);
    for (const auto& migratedLoop : migrated->document.at ("loops"))
    {
        CHECK (migratedLoop.at ("practiceSpeedPercent") == 100);
        CHECK (migratedLoop.at ("name") == "Solo");
    }
    CHECK (migrated->document.at ("activeLoopId") == "loop-1");
    CHECK (migrated->document.at ("looping") == true);
}
