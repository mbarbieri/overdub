#include "song/SongMigration.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using nlohmann::json;
using overdub::song::MigrationStep;
using overdub::song::migrateDocument;

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

TEST_CASE ("a v1 Song needs no migration in this build", "[song][migration]")
{
    const json document = { { "formatVersion", 1 }, { "sampleRate", 48000 } };

    const auto migrated = overdub::song::migrateSongDocument (document);

    REQUIRE (migrated.has_value());
    CHECK (migrated->fromVersion == 1);
    CHECK (migrated->document == document);
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
    const auto migrated = overdub::song::migrateSongDocument ({ { "formatVersion", 7 } });

    REQUIRE_FALSE (migrated.has_value());
    CHECK (migrated.error().code == "song.format_too_new");
    CHECK (migrated.error().fields.at ("versionFound") == 7);
    CHECK (migrated.error().fields.at ("versionSupported") == 1);
}

TEST_CASE ("a document without an integer formatVersion is refused", "[song][migration]")
{
    const auto migrated = overdub::song::migrateSongDocument ({ { "formatVersion", "one" } });

    REQUIRE_FALSE (migrated.has_value());
    CHECK (migrated.error().code == "song.json_invalid");
    CHECK (migrated.error().fields.at ("path") == "$.formatVersion");
}

TEST_CASE ("a failing step stops migration with an Error naming the version it started from", "[song][migration]")
{
    const std::vector<MigrationStep> steps { failingStep };

    const auto migrated = migrateDocument ({ { "formatVersion", 1 } }, steps);

    REQUIRE_FALSE (migrated.has_value());
    CHECK (migrated.error().code == "song.migration_failed");
    CHECK (migrated.error().fields.at ("fromVersion") == 1);
    CHECK (migrated.error().fields.at ("stepError") == "song.test_step_failed");
}
