#include "song/SongPackage.h"

#include "song/PluginBlobStore.h"
#include "song/SongJson.h"
#include "song/SongMigration.h"

#include <cerrno>
#include <sys/clonefile.h>
#include <utility>

namespace overdub::song
{
namespace
{
constexpr const char* mediaFolders[] = { "audio", "backing", "video", "plugins" };

// Reads, migrates and converts song.json. Logs song.migrated when the document was older.
core::Expected<Song> readSongJson (const juce::File& root)
{
    const auto file = root.getChildFile ("song.json");
    const auto text = readTextFile (file);
    if (! text.has_value())
        return std::unexpected (text.error());
    const auto parsed = parseJsonText (*text);
    if (! parsed.has_value())
        return std::unexpected (parsed.error());
    const auto migrated = migrateSongDocument (*parsed);
    if (! migrated.has_value())
        return std::unexpected (migrated.error());

    if (migrated->fromVersion != songFormatVersion)
        core::logEvent (core::Level::info, "song.migrated", { { "path", pathOf (root) }, { "fromVersion", migrated->fromVersion }, { "toVersion", songFormatVersion } });
    return songFromJson (migrated->document);
}

// Loads the history, or drops it and starts a fresh one from `song` (adding a Notice).
core::Expected<SongHistory> loadOrDropHistory (const juce::File& root, const Song& song, const BeforeRenameHook& beforeRename, std::vector<Notice>& notices)
{
    const auto directory = root.getChildFile ("history");
    auto loaded = SongHistory::load (directory, beforeRename);
    if (loaded.has_value())
        return loaded;

    auto fields = loaded.error().fields;
    fields["package"] = pathOf (root);
    core::logEvent (core::Level::warn, "song.history_dropped", fields, loaded.error().message);
    notices.push_back (Notice { "song.history_dropped", "Undo history could not be read, so it was cleared. The Song itself is intact." });
    return SongHistory::startFresh (directory, song, beforeRename);
}
} // namespace

SongPackage::SongPackage (juce::File packageRoot, Song song, SongHistory songHistory, PackageOptions packageOptions)
    : root (std::move (packageRoot)),
      currentSong (std::move (song)),
      history (std::move (songHistory)),
      options (std::move (packageOptions))
{
}

SongPackage::~SongPackage()
{
    if (auto written = flush(); ! written.has_value())
        core::logError (written.error());
}

core::Expected<std::unique_ptr<SongPackage>> SongPackage::create (const juce::File& location, Song song, PackageOptions options)
{
    if (location.exists())
        return std::unexpected (core::Error { "song.package_exists", "something already exists at the chosen location", { { "path", pathOf (location) } } });

    for (const auto* folder : mediaFolders)
        if (const auto created = location.getChildFile (folder).createDirectory(); created.failed())
            return std::unexpected (core::Error {
                "song.create_failed",
                "could not create the Song package",
                { { "path", pathOf (location) }, { "reason", created.getErrorMessage().toStdString() } },
            });

    if (auto written = writeJsonAtomically (location.getChildFile ("song.json"), songToJson (song), options.beforeRename); ! written.has_value())
        return std::unexpected (written.error());
    auto history = SongHistory::startFresh (location.getChildFile ("history"), song, options.beforeRename);
    if (! history.has_value())
        return std::unexpected (history.error());

    core::logEvent (core::Level::info, "song.created", { { "path", pathOf (location) }, { "sampleRate", song.sampleRate } });
    return std::unique_ptr<SongPackage> (new SongPackage (location, std::move (song), std::move (*history), std::move (options)));
}

core::Expected<SongPackage::Opened> SongPackage::open (const juce::File& location, PackageOptions options)
{
    if (! location.getChildFile ("song.json").existsAsFile())
        return std::unexpected (core::Error { "song.package_not_found", "no Song package at this location", { { "path", pathOf (location) } } });

    const int tempFilesDeleted = deleteTempFiles (location);

    const auto songJson = readSongJson (location);
    if (! songJson.has_value())
        return std::unexpected (songJson.error());

    std::vector<Notice> notices;
    auto history = loadOrDropHistory (location, *songJson, options.beforeRename, notices);
    if (! history.has_value())
        return std::unexpected (history.error());

    // Write order (edit, undo, redo): index.json commits the step, then song.json follows. After
    // a crash between the two, history holds the undoable content and song.json the rest.
    auto song = keepStateOutsideHistory (history->currentSong(), *songJson);
    const bool restoredFromHistory = song != *songJson;
    if (restoredFromHistory)
        core::logEvent (core::Level::warn, "song.restored_from_history", { { "path", pathOf (location) } });

    auto referencedHashes = history->pluginStateHashes();
    referencedHashes.merge (pluginStateHashes (song));
    const int blobsDeleted = deleteUnreferencedPluginBlobs (location.getChildFile ("plugins"), referencedHashes);

    auto package = std::unique_ptr<SongPackage> (new SongPackage (location, std::move (song), std::move (*history), std::move (options)));

    // A migrated song.json is written back at once, so the file on disk is at the current version.
    // (Kept unconditional and cheap: at v1 the document is rewritten unchanged.)
    package->writeDeadline = Clock::time_point {};
    if (auto written = package->flush(); ! written.has_value())
        return std::unexpected (written.error());

    core::logEvent (core::Level::info,
                    "song.opened",
                    {
                        { "path", pathOf (location) },
                        { "formatVersion", songFormatVersion },
                        { "tracks", package->currentSong.tracks.size() },
                        { "undoSteps", package->history.undoStepCount() },
                        { "tempFilesDeleted", tempFilesDeleted },
                        { "pluginBlobsDeleted", blobsDeleted },
                    });
    return Opened { std::move (package), std::move (notices) };
}

core::Expected<void> SongPackage::edit (Song changed, std::string label, Clock::time_point now)
{
    if (auto pushed = history.push (changed, label); ! pushed.has_value())
        return pushed;
    currentSong = std::move (changed);
    return writeCommittedStep (now);
}

void SongPackage::changeWithoutHistory (Song changed, Clock::time_point now)
{
    currentSong = std::move (changed);
    scheduleWrite (now);
}

core::Expected<void> SongPackage::undo (Clock::time_point now)
{
    return restoreSnapshot (history.undo(), now);
}

core::Expected<void> SongPackage::redo (Clock::time_point now)
{
    return restoreSnapshot (history.redo(), now);
}

core::Expected<void> SongPackage::restoreSnapshot (core::Expected<Song> snapshot, Clock::time_point now)
{
    if (! snapshot.has_value())
        return std::unexpected (snapshot.error());
    currentSong = keepStateOutsideHistory (std::move (*snapshot), currentSong);
    return writeCommittedStep (now);
}

core::Expected<void> SongPackage::writeCommittedStep (Clock::time_point now)
{
    // The step is already committed in history; a failed write stays pending and is retried.
    if (auto written = writeSongJson(); ! written.has_value())
    {
        scheduleWrite (now);
        return written;
    }
    return {};
}

core::Expected<void> SongPackage::writeIfDue (Clock::time_point now)
{
    if (! writeDeadline.has_value() || now < *writeDeadline)
        return {};
    return writeSongJson();
}

core::Expected<void> SongPackage::flush()
{
    if (! writeDeadline.has_value())
        return {};
    return writeSongJson();
}

core::Expected<std::string> SongPackage::storePluginState (const juce::MemoryBlock& state)
{
    return storePluginBlob (root.getChildFile ("plugins"), state, options.beforeRename);
}

core::Expected<juce::MemoryBlock> SongPackage::loadPluginState (const std::string& hash) const
{
    return loadPluginBlob (root.getChildFile ("plugins"), hash);
}

core::Expected<void> SongPackage::duplicateTo (const juce::File& destination)
{
    if (auto written = flush(); ! written.has_value())
        return written;

    if (::clonefile (root.getFullPathName().toRawUTF8(), destination.getFullPathName().toRawUTF8(), 0) != 0)
    {
        const int cloneErrno = errno;
        return std::unexpected (core::Error {
            "song.duplicate_failed",
            "could not duplicate the Song",
            { { "path", pathOf (root) }, { "destination", pathOf (destination) }, { "errno", cloneErrno } },
        });
    }
    core::logEvent (core::Level::info, "song.duplicated", { { "path", pathOf (root) }, { "destination", pathOf (destination) } });
    return {};
}

void SongPackage::scheduleWrite (Clock::time_point now)
{
    if (! writeDeadline.has_value())
        writeDeadline = now + options.saveDelay;
}

core::Expected<void> SongPackage::writeSongJson()
{
    const auto file = root.getChildFile ("song.json");
    const auto written = writeJsonAtomically (file, songToJson (currentSong), options.beforeRename);
    if (! written.has_value())
        return std::unexpected (written.error());

    writeDeadline.reset();
    core::logEvent (core::Level::debug, "song.saved", { { "path", pathOf (file) }, { "bytes", *written } });
    return {};
}
} // namespace overdub::song
