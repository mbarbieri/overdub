#pragma once

#include "core/Error.h"
#include "song/AtomicWrite.h"
#include "song/Song.h"

#include <juce_core/juce_core.h>

#include <cstdint>
#include <set>
#include <string>
#include <vector>

// Persisted, linear, whole-Song snapshot history (ADR 0004), kept in a package's history/ folder.
//
// - history/NNNNNN.json is { "label": <step label>, "song": <whole Song document> }. NNNNNN is a
//   zero-padded step number that only ever grows; numbers are never reused or renumbered.
// - The oldest snapshot is the baseline: the Song before the oldest undoable step. Creating a
//   Song, or dropping unreadable history, writes a baseline labelled "".
// - history/index.json is { "oldest", "newest", "current" }: step numbers. Undo moves current
//   down, redo up. Snapshot files outside [oldest, newest] are stray (an interrupted prune) and
//   are deleted on load.
// - At most maxUndoSteps steps: pushing one more deletes the oldest snapshot (the next one becomes
//   the baseline). Pushing after an undo deletes the redo branch's files.
// - Snapshot files are never rewritten: an older formatVersion is migrated in memory on every load.
// Write order keeps a crash at any point loadable: snapshot file, then index.json, then deletions.
namespace overdub::song
{
class SongHistory
{
public:
    static constexpr int maxUndoSteps = 200;

    // Reads index.json and every snapshot, migrating each. Any problem (missing folder, unreadable
    // index, missing, corrupt or unmigratable snapshot) is an Error: the caller drops the history.
    // Errors: song.history_unreadable: `path` is the history file that failed, `cause` the
    // underlying Error code (song.read_failed, song.json_unparsable, song.json_invalid,
    // song.history_index_invalid, song.format_too_new, song.migration_failed), plus the cause's
    // own fields. A stray snapshot file that cannot be deleted is logged as
    // song.snapshot_delete_failed (warn, path).
    static core::Expected<SongHistory> load (const juce::File& directory, BeforeRenameHook beforeRename);

    // Deletes `directory` and starts it over with `baseline` as the only snapshot.
    // Errors: song.write_failed (step clear_folder|create_folder, or a file write's),
    // song.json_unserializable.
    static core::Expected<SongHistory> startFresh (const juce::File& directory, const Song& baseline, BeforeRenameHook beforeRename);

    // Adds a step after the current one, deleting any redo branch and anything over the cap. The
    // step is committed once index.json is written; on an Error nothing changed in memory.
    // Errors: song.write_failed, song.json_unserializable (writeJsonAtomically).
    core::Expected<void> push (const Song& song, const std::string& label);

    bool canUndo() const { return current > oldest; }
    bool canRedo() const { return current < newest; }
    std::string undoLabel() const; // the label of the step undo would revert; "" if none
    std::string redoLabel() const; // the label of the step redo would apply; "" if none

    // Move one step and return the snapshot that is now current. The move is committed once
    // index.json is written; on an Error nothing changed. Errors: song.history_empty, plus
    // writeJsonAtomically's.
    core::Expected<Song> undo();
    core::Expected<Song> redo();

    // The current step's snapshot: the Song's undoable content as history last committed it.
    const Song& currentSong() const;

    // Every plugin state hash any snapshot references.
    std::set<std::string> pluginStateHashes() const;

    int undoStepCount() const { return static_cast<int> (current - oldest); }

private:
    struct Snapshot
    {
        std::string label;
        Song song;
    };

    SongHistory (juce::File directory, BeforeRenameHook beforeRename);

    const Snapshot& snapshotAt (std::int64_t number) const;
    juce::File snapshotFile (std::int64_t number) const;
    core::Expected<void> writeSnapshot (std::int64_t number, const Snapshot& snapshot) const;
    core::Expected<void> writeIndex (std::int64_t oldestStep, std::int64_t newestStep, std::int64_t currentStep) const;
    core::Expected<Song> moveTo (std::int64_t step);
    void deleteSnapshotFilesOutsideRange() const;

    juce::File directory;
    BeforeRenameHook beforeRename;
    std::vector<Snapshot> snapshots; // snapshots[i] is step number oldest + i
    std::int64_t oldest = 0;
    std::int64_t newest = 0;
    std::int64_t current = 0;
};
} // namespace overdub::song
