#pragma once

#include "core/Error.h"
#include "song/Song.h"

#include <juce_core/juce_core.h>

#include <nlohmann/json.hpp>

#include <functional>
#include <span>
#include <string_view>

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
// Errors: song.json_invalid (no integer formatVersion, json_path "$.formatVersion"),
// song.format_too_new (version_found, version_supported), song.migration_failed (from_version,
// step_error: the failing step's code).
core::Expected<MigratedDocument> migrateDocument (nlohmann::json document, std::span<const MigrationStep> steps);

// This build's steps, one per formatVersion after 1; add one per new formatVersion and bump
// songFormatVersion.
//   1 -> 2 (issue #68): every Video Take gains "outputDevice": "" (unknown).
//   2 -> 3 (issue #114): every Backing Track gains "separatedFrom": { "backingTrackId": "",
//           "model": "" } (imported, not separated).
//   3 -> 4 (issue #137): every Track gains a "colour": trackColourPalette[0], [1], ... in Song
//           order, wrapping after the last (deterministic, no randomness).
//   4 -> 5 (issue #201): the Song gains "exportFormat": "wav", "exportLength":
//           "trim_to_video_take" and "exportDestination": "" (the export folder).
//   5 -> 6 (issue #209): the Song gains "transpose": 0.
std::span<const MigrationStep> songMigrationSteps();

// migrateDocument with songMigrationSteps().
core::Expected<MigratedDocument> migrateSongDocument (nlohmann::json document);

// A Song read from a package file (song.json or a history snapshot) at any supported version.
struct StoredSong
{
    nlohmann::json file; // the whole parsed file, as on disk
    Song song;           // converted after migrating to songFormatVersion
    int fromVersion = 0; // the formatVersion on disk
};

// The one read path for package Songs: read, parse, migrate, convert. The Song document is the
// file's member `songKey`, or the whole file when `songKey` is empty. Every Error carries the
// file's `path`. Errors: song.read_failed, song.json_unparsable, song.json_invalid,
// song.format_too_new, song.migration_failed.
core::Expected<StoredSong> readStoredSong (const juce::File& file, std::string_view songKey);
} // namespace overdub::song
