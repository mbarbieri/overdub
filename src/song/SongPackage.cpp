#include "song/SongPackage.h"

#include "song/BackingTrackImport.h"
#include "song/PackageFolders.h"
#include "song/PluginBlobStore.h"
#include "song/Separation.h"
#include "song/SongJson.h"
#include "song/SongMigration.h"

#include <algorithm>
#include <cerrno>
#include <sys/clonefile.h>
#include <utility>

namespace overdub::song
{
namespace
{
constexpr const char* mediaFolders[] = { audioFolderName, backingFolderName, videoFolderName, pluginsFolderName };

struct SongJsonOnDisk
{
    Song song;
    bool migrated = false; // the file is at an older formatVersion
};

// Reads, migrates and converts song.json. Logs song.migrated when the document was older.
core::Expected<SongJsonOnDisk> readSongJson (const juce::File& root)
{
    const auto file = root.getChildFile ("song.json");
    auto stored = readStoredSong (file, "");
    if (! stored.has_value())
        return std::unexpected (stored.error());

    const bool wasMigrated = stored->fromVersion != songFormatVersion;
    if (wasMigrated)
        core::logEvent (core::Level::info, "song.migrated", { { "path", pathOf (file) }, { "from_version", stored->fromVersion }, { "to_version", songFormatVersion } });
    return SongJsonOnDisk { std::move (stored->song), wasMigrated };
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
    notices.push_back (Notice { "song.history_dropped" });
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
                { { "path", pathOf (location) }, { "folder", folder }, { "reason", created.getErrorMessage().toStdString() } },
            });

    if (auto written = writeJsonAtomically (location.getChildFile ("song.json"), songToJson (song), options.beforeRename); ! written.has_value())
        return std::unexpected (written.error());
    auto history = SongHistory::startFresh (location.getChildFile ("history"), song, options.beforeRename);
    if (! history.has_value())
        return std::unexpected (history.error());

    core::logEvent (core::Level::info, "song.created", { { "path", pathOf (location) }, { "sample_rate", song.sampleRate } });
    return std::unique_ptr<SongPackage> (new SongPackage (location, std::move (song), std::move (*history), std::move (options)));
}

core::Expected<SongPackage::Opened> SongPackage::open (const juce::File& location, PackageOptions options)
{
    if (! location.getChildFile ("song.json").existsAsFile())
        return std::unexpected (core::Error { "song.package_not_found", "no Song package at this location", { { "path", pathOf (location) } } });

    // Read (and so check formatVersion) before touching anything: a refused package is left as is.
    const auto songJson = readSongJson (location);
    if (! songJson.has_value())
        return std::unexpected (songJson.error());

    const int tempFilesDeleted = deleteTempFiles (location);

    std::vector<Notice> notices;
    auto history = loadOrDropHistory (location, songJson->song, options.beforeRename, notices);
    if (! history.has_value())
        return std::unexpected (history.error());

    // Write order (edit, undo, redo): index.json commits the step, then song.json follows. After
    // a crash between the two, history holds the undoable content and song.json the rest.
    auto song = withContentOf (history->currentSong(), songJson->song);
    const bool restoredFromHistory = song != songJson->song;
    if (restoredFromHistory)
        core::logEvent (core::Level::warn, "song.restored_from_history", { { "path", pathOf (location) } });

    auto referencedHashes = history->pluginStateHashes();
    referencedHashes.merge (pluginStateHashes (song));
    const int blobsDeleted = deleteUnreferencedPluginBlobs (location.getChildFile (pluginsFolderName), referencedHashes);

    if (const auto missing = repairBackingTrackFiles (location, song, options.beforeRename); ! missing.empty())
        notices.push_back (Notice { "song.backing_track_missing" });

    auto package = std::unique_ptr<SongPackage> (new SongPackage (location, std::move (song), std::move (*history), std::move (options)));

    // Rewritten only when it differs from what is on disk: migrated, or restored from history.
    if (songJson->migrated || restoredFromHistory)
        if (auto written = package->writeSongJson(); ! written.has_value())
            return std::unexpected (written.error());

    core::logEvent (core::Level::info,
                    "song.opened",
                    {
                        { "path", pathOf (location) },
                        { "format_version", songFormatVersion },
                        { "tracks", package->currentSong.tracks.size() },
                        { "undo_steps", package->history.undoStepCount() },
                        { "temp_files_deleted", tempFilesDeleted },
                        { "plugin_blobs_deleted", blobsDeleted },
                    });
    return Opened { std::move (package), std::move (notices) };
}

core::Expected<void> SongPackage::edit (Song changed, std::string label, Clock::time_point now)
{
    return commitEdit (std::move (changed), std::move (label), now).result;
}

SongPackage::EditOutcome SongPackage::commitEdit (Song changed, std::string label, Clock::time_point now)
{
    capturePluginState (changed);
    if (auto pushed = history.push (changed, label); ! pushed.has_value())
        return EditOutcome { false, std::move (pushed) };
    currentSong = std::move (changed);
    return EditOutcome { true, writeCommittedStep (now) };
}

void SongPackage::changeWithoutHistory (SongSettings changed, Clock::time_point now)
{
    currentSong.settings = std::move (changed);
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
    // The surviving slots keep their current state (withContentOf): make it current.
    capturePluginState (currentSong);
    currentSong = withContentOf (std::move (*snapshot), currentSong);
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
    capturePluginState (currentSong);
    return writeSongJson();
}

core::Expected<void> SongPackage::flush()
{
    if (! writeDeadline.has_value())
        return {};
    capturePluginState (currentSong);
    return writeSongJson();
}

core::Expected<std::string> SongPackage::storePluginState (const juce::MemoryBlock& state)
{
    return storePluginBlob (root.getChildFile (pluginsFolderName), state, options.beforeRename);
}

core::Expected<juce::MemoryBlock> SongPackage::loadPluginState (const std::string& hash) const
{
    return loadPluginBlob (root.getChildFile (pluginsFolderName), hash);
}

core::Expected<Id> SongPackage::importBackingTrack (const juce::File& source, Clock::time_point now)
{
    auto backingTrack = writeBackingTrackFiles (root, source, currentSong.sampleRate, options.beforeRename);
    if (! backingTrack.has_value())
        return std::unexpected (backingTrack.error());

    auto edited = commitEdit (withImportedBackingTrack (currentSong, *backingTrack, options.pickTrackColour), importStepLabel (*backingTrack), now);
    if (! edited.committed)
    {
        deleteBackingTrackFiles (root, *backingTrack);
        return std::unexpected (edited.result.error());
    }

    // Committed: the import stands even when its song.json write failed (that write stays pending).
    core::logEvent (core::Level::info,
                    "song.backing_track_imported",
                    {
                        { "backing_track_id", backingTrack->id },
                        { "source", pathOf (source) },
                        { "format_name", backingTrack->formatName },
                        { "original_sample_rate", backingTrack->originalSampleRate },
                        { "sample_rate", currentSong.sampleRate },
                        { "channels", backingTrack->channels },
                        { "length_samples", backingTrack->lengthSamples },
                    });
    if (! edited.result.has_value())
        return std::unexpected (edited.result.error());
    return backingTrack->id;
}

std::vector<core::Expected<Id>> SongPackage::importBackingTracks (const std::vector<juce::File>& sources, Clock::time_point now)
{
    std::vector<core::Expected<Id>> results;
    for (const auto& source : sources)
        results.push_back (importBackingTrack (source, now));
    return results;
}

core::Expected<std::vector<Id>> SongPackage::addSeparation (const Id& sourceTrackId, const std::vector<juce::File>& partFiles, const std::string& model, Clock::time_point now)
{
    const auto sourceId = separableBackingTrack (currentSong, sourceTrackId);
    if (! sourceId.has_value())
        return std::unexpected (sourceId.error());
    if (partFiles.size() != separationParts.size())
        return std::unexpected (core::Error {
            "song.separation_parts_invalid",
            "a Separation needs one file per part",
            { { "expected", separationParts.size() }, { "found", partFiles.size() } },
        });

    std::vector<BackingTrack> parts;
    const auto deleteWritten = [this, &parts] {
        for (const auto& part : parts)
            deleteBackingTrackFiles (root, part);
    };
    for (const auto& partFile : partFiles)
    {
        auto part = writeBackingTrackFiles (root, partFile, currentSong.sampleRate, options.beforeRename);
        if (! part.has_value())
        {
            deleteWritten();
            return std::unexpected (part.error());
        }
        part->separatedFrom = SeparationOrigin { *sourceId, model };
        parts.push_back (std::move (*part));
    }

    const auto source = std::ranges::find (currentSong.backingTracks, *sourceId, &BackingTrack::id);
    const auto label = separationStepLabel (*source);
    auto changed = withSeparation (currentSong, sourceTrackId, parts, options.pickTrackColour);
    if (! changed.has_value())
    {
        deleteWritten();
        return std::unexpected (changed.error());
    }
    auto edited = commitEdit (std::move (*changed), label, now);
    if (! edited.committed)
    {
        deleteWritten();
        return std::unexpected (edited.result.error());
    }

    // Committed: the Separation stands even when its song.json write failed (that write stays pending).
    if (! edited.result.has_value())
        return std::unexpected (edited.result.error());
    std::vector<Id> ids;
    for (const auto& part : parts)
        ids.push_back (part.id);
    return ids;
}

bool SongPackage::isBackingTrackMissing (const Id& backingTrackId) const
{
    const auto backingTrack = std::ranges::find (currentSong.backingTracks, backingTrackId, &BackingTrack::id);
    return backingTrack == currentSong.backingTracks.end() || ! derivedFileFor (root, *backingTrack).existsAsFile();
}

core::Expected<Peaks> SongPackage::peaks (const ClipSource& source)
{
    const auto mediaFile = mediaFileFor (root, currentSong, source);
    if (! mediaFile.has_value())
        return std::unexpected (core::Error {
            "song.media_unknown",
            "the Song has no such Take or Backing Track",
            { { "source_kind", toString (source.kind) }, { "source_id", source.id } },
        });
    return loadOrRebuildPeaks (root, source.id, *mediaFile, options.beforeRename);
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

void SongPackage::capturePluginState (Song& song)
{
    if (pluginStateCapture)
        pluginStateCapture (song);
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
