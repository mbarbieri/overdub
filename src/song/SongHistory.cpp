#include "song/SongHistory.h"

#include "song/SongJson.h"
#include "song/SongMigration.h"

#include <algorithm>
#include <cerrno>
#include <optional>
#include <utility>

namespace overdub::song
{
namespace
{
// Wraps the reason history could not be loaded. Keeps the cause's fields (its path, json_path,
// byte, versions...) and adds its code as `cause`. `path` is the history file that failed.
core::Error unreadable (const juce::File& file, const core::Error& cause)
{
    auto fields = cause.fields.is_object() ? cause.fields : nlohmann::json::object();
    fields["path"] = pathOf (file);
    fields["cause"] = cause.code;
    return core::Error { "song.history_unreadable", "undo history could not be read: " + cause.message, std::move (fields) };
}

// Reads one integer field of index.json, or nullopt.
std::optional<std::int64_t> indexField (const nlohmann::json& index, const char* key)
{
    if (! index.is_object() || ! index.contains (key) || ! index.at (key).is_number_integer())
        return std::nullopt;
    return index.at (key).get<std::int64_t>();
}

struct StepRange
{
    std::int64_t oldest = 0;
    std::int64_t newest = 0;
    std::int64_t current = 0;
};

// Reads index.json. Errors: song.read_failed, song.json_unparsable, song.history_index_invalid.
core::Expected<StepRange> readIndex (const juce::File& indexFile, std::int64_t maxSteps)
{
    const auto index = readJsonFile (indexFile);
    if (! index.has_value())
        return std::unexpected (index.error());

    const auto oldest = indexField (*index, "oldest");
    const auto newest = indexField (*index, "newest");
    const auto current = indexField (*index, "current");
    const bool consistent = oldest && newest && current && *oldest >= 0 && *oldest <= *current && *current <= *newest
                         && *newest - *oldest <= maxSteps;
    if (! consistent)
        return std::unexpected (core::Error {
            "song.history_index_invalid",
            "index.json does not hold a valid step range",
            { { "path", pathOf (indexFile) }, { "index", index->dump() } },
        });
    return StepRange { *oldest, *newest, *current };
}

// Reads one snapshot file through the package's one Song read path.
core::Expected<std::pair<std::string, Song>> readSnapshot (const juce::File& file)
{
    auto stored = readStoredSong (file, "song");
    if (! stored.has_value())
        return std::unexpected (stored.error());

    const auto label = stored->file.find ("label");
    if (label == stored->file.end() || ! label->is_string())
        return std::unexpected (withFilePath (core::Error { "song.json_invalid", "snapshot has no label", { { "json_path", "$.label" }, { "reason", "expected a string" } } }, file));
    return std::pair { label->get<std::string>(), std::move (stored->song) };
}
} // namespace

SongHistory::SongHistory (juce::File historyDirectory, BeforeRenameHook hook)
    : directory (std::move (historyDirectory)),
      beforeRename (std::move (hook))
{
}

core::Expected<SongHistory> SongHistory::load (const juce::File& directory, BeforeRenameHook beforeRename)
{
    const auto indexFile = directory.getChildFile ("index.json");
    const auto range = readIndex (indexFile, maxUndoSteps);
    if (! range.has_value())
        return std::unexpected (unreadable (indexFile, range.error()));

    SongHistory history (directory, std::move (beforeRename));
    history.oldest = range->oldest;
    history.newest = range->newest;
    history.current = range->current;
    for (auto number = range->oldest; number <= range->newest; ++number)
    {
        const auto file = history.snapshotFile (number);
        auto snapshot = readSnapshot (file);
        if (! snapshot.has_value())
            return std::unexpected (unreadable (file, snapshot.error()));
        history.snapshots.push_back (Snapshot { std::move (snapshot->first), std::move (snapshot->second) });
    }

    history.deleteSnapshotFilesOutsideRange();
    return history;
}

core::Expected<SongHistory> SongHistory::startFresh (const juce::File& directory, const Song& baseline, BeforeRenameHook beforeRename)
{
    // errno is read right after the JUCE call; JUCE's last failing syscall sets it.
    if (directory.exists() && ! directory.deleteRecursively())
        return std::unexpected (writeFailed (directory, "clear_folder", errno));
    if (directory.createDirectory().failed())
        return std::unexpected (writeFailed (directory, "create_folder", errno));

    SongHistory history (directory, std::move (beforeRename));
    history.snapshots.push_back (Snapshot { "", baseline });
    if (auto written = history.writeSnapshot (0, history.snapshots.back()); ! written.has_value())
        return std::unexpected (written.error());
    if (auto written = history.writeIndex (0, 0, 0); ! written.has_value())
        return std::unexpected (written.error());
    return history;
}

core::Expected<void> SongHistory::push (const Song& song, const std::string& label)
{
    const auto number = current + 1;
    const auto newOldest = std::max (oldest, number - maxUndoSteps);
    Snapshot snapshot { label, song };
    if (auto written = writeSnapshot (number, snapshot); ! written.has_value())
        return written;
    if (auto written = writeIndex (newOldest, number, number); ! written.has_value())
        return written;

    // Committed on disk. In memory: drop the redo branch, append, then trim to the cap.
    snapshots.resize (static_cast<std::size_t> (current - oldest + 1));
    snapshots.push_back (std::move (snapshot));
    snapshots.erase (snapshots.begin(), snapshots.begin() + (newOldest - oldest));
    oldest = newOldest;
    current = number;
    newest = number;

    deleteSnapshotFilesOutsideRange();
    return {};
}

std::string SongHistory::undoLabel() const
{
    return canUndo() ? snapshotAt (current).label : std::string();
}

std::string SongHistory::redoLabel() const
{
    return canRedo() ? snapshotAt (current + 1).label : std::string();
}

core::Expected<Song> SongHistory::undo()
{
    if (! canUndo())
        return std::unexpected (core::Error { "song.history_empty", "nothing to undo", { { "direction", "undo" } } });
    return moveTo (current - 1);
}

core::Expected<Song> SongHistory::redo()
{
    if (! canRedo())
        return std::unexpected (core::Error { "song.history_empty", "nothing to redo", { { "direction", "redo" } } });
    return moveTo (current + 1);
}

core::Expected<Song> SongHistory::moveTo (std::int64_t step)
{
    if (auto written = writeIndex (oldest, newest, step); ! written.has_value())
        return std::unexpected (written.error());
    current = step;
    return snapshotAt (current).song;
}

const Song& SongHistory::currentSong() const
{
    return snapshotAt (current).song;
}

std::set<std::string> SongHistory::pluginStateHashes() const
{
    std::set<std::string> hashes;
    for (const auto& snapshot : snapshots)
        hashes.merge (song::pluginStateHashes (snapshot.song));
    return hashes;
}

const SongHistory::Snapshot& SongHistory::snapshotAt (std::int64_t number) const
{
    // Callers stay within [oldest, newest]; snapshots is never empty.
    if (! OVERDUB_CHECK (number >= oldest && number <= newest, "song.history_step_out_of_range", { "step", number }, { "oldest", oldest }, { "newest", newest }))
        number = std::clamp (number, oldest, newest);
    return snapshots[static_cast<std::size_t> (number - oldest)];
}

juce::File SongHistory::snapshotFile (std::int64_t number) const
{
    return directory.getChildFile (juce::String (number).paddedLeft ('0', 6) + ".json");
}

core::Expected<void> SongHistory::writeSnapshot (std::int64_t number, const Snapshot& snapshot) const
{
    const nlohmann::json document { { "label", snapshot.label }, { "song", songToJson (snapshot.song) } };
    if (auto written = writeJsonAtomically (snapshotFile (number), document, beforeRename); ! written.has_value())
        return std::unexpected (written.error());
    return {};
}

core::Expected<void> SongHistory::writeIndex (std::int64_t oldestStep, std::int64_t newestStep, std::int64_t currentStep) const
{
    const nlohmann::json index { { "oldest", oldestStep }, { "newest", newestStep }, { "current", currentStep } };
    if (auto written = writeJsonAtomically (directory.getChildFile ("index.json"), index, beforeRename); ! written.has_value())
        return std::unexpected (written.error());
    return {};
}

void SongHistory::deleteSnapshotFilesOutsideRange() const
{
    for (const auto& file : directory.findChildFiles (juce::File::findFiles, false, "*.json"))
    {
        const auto name = file.getFileNameWithoutExtension();
        if (name == "index" || ! name.containsOnly ("0123456789"))
            continue;
        const auto number = name.getLargeIntValue();
        if ((number < oldest || number > newest) && ! file.deleteFile())
            core::logEvent (core::Level::warn, "song.snapshot_delete_failed", { { "path", pathOf (file) } });
    }
}
} // namespace overdub::song
