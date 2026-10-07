#include "song/SongMigration.h"

namespace overdub::song
{
core::Expected<MigratedDocument> migrateDocument (nlohmann::json document, std::span<const MigrationStep> steps)
{
    const auto version = document.is_object() ? document.find ("formatVersion") : document.end();
    if (! document.is_object() || version == document.end() || ! version->is_number_integer())
        return std::unexpected (core::Error {
            "song.json_invalid",
            "song document has no integer formatVersion",
            { { "json_path", "$.formatVersion" }, { "reason", "expected an integer" } },
        });

    const int fromVersion = version->get<int>();
    const int newestVersion = static_cast<int> (steps.size()) + 1;
    if (fromVersion > newestVersion)
        return std::unexpected (core::Error {
            "song.format_too_new",
            "song was written by a newer Overdub (format " + std::to_string (fromVersion) + ")",
            { { "version_found", fromVersion }, { "version_supported", newestVersion } },
        });
    if (fromVersion < 1)
        return std::unexpected (core::Error {
            "song.json_invalid",
            "song formatVersion is not a known version",
            { { "json_path", "$.formatVersion" }, { "reason", "expected 1 or more" } },
        });

    for (int stepVersion = fromVersion; stepVersion < newestVersion; ++stepVersion)
    {
        auto migrated = steps[static_cast<std::size_t> (stepVersion - 1)] (std::move (document));
        if (! migrated.has_value())
            return std::unexpected (core::Error {
                "song.migration_failed",
                "could not migrate song from format " + std::to_string (stepVersion),
                { { "from_version", stepVersion }, { "step_error", migrated.error().code } },
            });
        document = std::move (*migrated);
        document["formatVersion"] = stepVersion + 1;
    }
    return MigratedDocument { std::move (document), fromVersion };
}

std::span<const MigrationStep> songMigrationSteps()
{
    // Empty at formatVersion 1. songFormatVersion must equal songMigrationSteps().size() + 1.
    return {};
}

core::Expected<MigratedDocument> migrateSongDocument (nlohmann::json document)
{
    return migrateDocument (std::move (document), songMigrationSteps());
}
} // namespace overdub::song
