#include "song/SongMigration.h"

#include "song/AtomicWrite.h"
#include "song/SongJson.h"

#include <array>

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
        auto migrated = steps[static_cast<std::size_t> (stepVersion - 1)](std::move (document));
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

namespace
{
core::Expected<nlohmann::json> addVideoTakeOutputDevice (nlohmann::json document)
{
    const auto videoTakes = document.find ("videoTakes");
    if (videoTakes == document.end() || ! videoTakes->is_array())
        return document; // reading the migrated document reports what is missing
    for (auto& videoTake : *videoTakes)
        if (videoTake.is_object())
            videoTake["outputDevice"] = "";
    return document;
}

core::Expected<nlohmann::json> addBackingTrackSeparatedFrom (nlohmann::json document)
{
    const auto backingTracks = document.find ("backingTracks");
    if (backingTracks == document.end() || ! backingTracks->is_array())
        return document; // reading the migrated document reports what is missing
    for (auto& backingTrack : *backingTracks)
        if (backingTrack.is_object())
            backingTrack["separatedFrom"] = { { "backingTrackId", "" }, { "model", "" } };
    return document;
}

core::Expected<nlohmann::json> addTrackColours (nlohmann::json document)
{
    const auto tracks = document.find ("tracks");
    if (tracks == document.end() || ! tracks->is_array())
        return document; // reading the migrated document reports what is missing
    std::size_t next = 0;
    for (auto& track : *tracks)
        if (track.is_object())
            track["colour"] = std::string (trackColourPalette[next++ % trackColourPalette.size()]);
    return document;
}
} // namespace

std::span<const MigrationStep> songMigrationSteps()
{
    // songFormatVersion must equal songMigrationSteps().size() + 1.
    static const std::array<MigrationStep, 3> steps { addVideoTakeOutputDevice, addBackingTrackSeparatedFrom, addTrackColours };
    static_assert (std::tuple_size_v<decltype (steps)> + 1 == songFormatVersion);
    return steps;
}

core::Expected<MigratedDocument> migrateSongDocument (nlohmann::json document)
{
    return migrateDocument (std::move (document), songMigrationSteps());
}

core::Expected<StoredSong> readStoredSong (const juce::File& file, std::string_view songKey)
{
    auto parsed = readJsonFile (file);
    if (! parsed.has_value())
        return std::unexpected (parsed.error());

    const bool wholeFile = songKey.empty();
    if (! wholeFile && (! parsed->is_object() || ! parsed->contains (songKey)))
        return std::unexpected (withFilePath (core::Error {
                                                  "song.json_invalid",
                                                  "package file has no song document",
                                                  { { "json_path", "$." + std::string (songKey) }, { "reason", "missing" } },
                                              },
                                              file));

    auto migrated = migrateSongDocument (wholeFile ? *parsed : parsed->at (songKey));
    if (! migrated.has_value())
        return std::unexpected (withFilePath (migrated.error(), file));
    auto song = songFromJson (migrated->document);
    if (! song.has_value())
        return std::unexpected (withFilePath (song.error(), file));
    return StoredSong { std::move (*parsed), std::move (*song), migrated->fromVersion };
}
} // namespace overdub::song
