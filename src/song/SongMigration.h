#pragma once

#include "core/Error.h"

#include <nlohmann/json.hpp>

#include <functional>
#include <span>

// The formatVersion migration scaffold (issue #8, ADR 0004). One step function per version moves a
// raw song.json document from version v to v + 1; the package runs it over song.json and every
// history snapshot. Steps work on JSON, not on the Song structs, because old documents do not fit
// the current structs. Newer versions than this build knows are refused.
namespace overdub::song
{
// Migrates a document from one version to the next. It need not touch formatVersion: the
// scaffold stamps it after the step succeeds.
using MigrationStep = std::function<core::Expected<nlohmann::json> (nlohmann::json)>;

struct MigratedDocument
{
    nlohmann::json document; // at the newest version
    int fromVersion = 0;     // the version the document had on disk
};

// steps[i] migrates version i + 1 to i + 2, so the newest version is steps.size() + 1.
// Errors: song.json_invalid (no integer formatVersion, path "$.formatVersion"),
// song.format_too_new (versionFound, versionSupported), song.migration_failed (fromVersion,
// stepError: the failing step's code).
core::Expected<MigratedDocument> migrateDocument (nlohmann::json document, std::span<const MigrationStep> steps);

// This build's steps: none yet at v1. Add one per new formatVersion and bump songFormatVersion.
std::span<const MigrationStep> songMigrationSteps();

// migrateDocument with songMigrationSteps().
core::Expected<MigratedDocument> migrateSongDocument (nlohmann::json document);
} // namespace overdub::song
