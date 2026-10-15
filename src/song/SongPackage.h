#pragma once

#include "core/Error.h"
#include "song/AtomicWrite.h"
#include "song/Peaks.h"
#include "song/Song.h"
#include "song/SongHistory.h"

#include <juce_core/juce_core.h>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// A Song on disk: the always-saved `.overdub` package with persisted undo history (ADR 0004).
//
//   <name>.overdub/
//     song.json                  the current Song, written atomically (temp + rename)
//     history/NNNNNN.json        { "label", "song" }: one whole-Song snapshot per step (SongHistory.h)
//     history/index.json         { "oldest", "newest", "current" }
//     plugins/<sha256>.state     plugin state blobs, content-addressed, written once
//     backing/<id>.orig.<ext>    an imported Backing Track's original file, untouched
//     backing/<id>.wav           its derived 32-bit float WAV at the Song's rate (BackingTrackImport.h)
//     peaks/<media-id>.peaks     the peaks cache of every Take and Backing Track (Peaks.h)
//     audio/ video/              Take and Video Take media, written by later slices
//
// There is no Save. Two change paths:
// - edit(), undo() and redo() write a history step and song.json at once, in this order:
//   snapshot file (edit only), index.json, song.json. index.json commits the step. A crash
//   before it reopens the previous step; a crash after it reopens the new step, because open()
//   takes the undoable content from history's current snapshot and only the state outside
//   history from song.json (keepStateOutsideHistory), then repairs song.json.
// - changeWithoutHistory() (Armed, input channels, plugin parameters, pre-roll, camera, playhead,
//   export choice) writes no history; its song.json write is debounced by saveDelay.
// Both capture the live plugin state first (setPluginStateCapture, issue #33).
//
// Single-threaded: every call comes from one thread (the message thread in the app).
namespace overdub::song
{
// A Notice-tier situation for the UI to show as its one-line notice (ADR 0006). Only the code:
// the sentence the user reads comes from ui/'s code-to-sentence table.
struct Notice
{
    std::string code; // same namespace as events, e.g. "song.history_dropped"
};

struct PackageOptions
{
    // The song.json write happens at most this long after the first unsaved change. The deadline
    // is not pushed back by later changes, so a stream of changes (playhead) still gets written.
    std::chrono::milliseconds saveDelay { 500 };

    // Test seam for every package write; see BeforeRenameHook in song/AtomicWrite.h.
    BeforeRenameHook beforeRename;

    // Picks the colour of each Track an import or a Separation adds (newTrackColour).
    PickIndex pickTrackColour = randomPick();
};

class SongPackage
{
public:
    using Clock = std::chrono::steady_clock;

    struct Opened;

    // Creates a new package at `location`, which must not exist yet. Errors: song.package_exists,
    // song.create_failed (path, folder, reason), song.write_failed, song.json_unserializable.
    static core::Expected<std::unique_ptr<SongPackage>> create (const juce::File& location, Song song, PackageOptions options = {});

    // Opens an existing package: reads and migrates song.json (any Error here leaves the package
    // untouched), then deletes leftover *.tmp files, loads and migrates every snapshot, drops
    // unreadable history (a Notice, never an Error), deletes unreferenced plugin blobs, and
    // rewrites song.json only if it was migrated or restored from history.
    // Then it rebuilds each missing derived Backing Track file from its original; a Backing Track
    // with neither is missing (song.backing_track_missing, plus a Notice): its Clips play silence.
    // Errors: song.package_not_found, song.read_failed, song.json_unparsable, song.json_invalid,
    // song.format_too_new, song.migration_failed (all with the file's path); song.write_failed,
    // song.json_unserializable (rewriting song.json, or starting a fresh history).
    static core::Expected<Opened> open (const juce::File& location, PackageOptions options = {});

    // Writes any pending change; a failure is logged.
    ~SongPackage();

    SongPackage (const SongPackage&) = delete;
    SongPackage& operator= (const SongPackage&) = delete;

    const Song& song() const { return currentSong; }
    const juce::File& location() const { return root; }

    // An undoable change: one history step named `label` ("Import Band Mix.mp3"), then song.json.
    // An Error from the history write leaves everything unchanged. An Error from the song.json
    // write comes after the step is committed: the change stays applied and the write pending.
    // Errors: song.write_failed, song.json_unserializable, or the beforeRename hook's.
    core::Expected<void> edit (Song changed, std::string label, Clock::time_point now = Clock::now());

    // A change that is saved (debounced) but not undoable.
    void changeWithoutHistory (Song changed, Clock::time_point now = Clock::now());

    bool canUndo() const { return history.canUndo(); }
    bool canRedo() const { return history.canRedo(); }
    std::string undoLabel() const { return history.undoLabel(); }
    std::string redoLabel() const { return history.redoLabel(); }

    // Restore the neighbouring snapshot, keeping state outside history (keepStateOutsideHistory),
    // then write song.json. Errors: song.history_empty (nothing to undo/redo), otherwise as edit().
    core::Expected<void> undo (Clock::time_point now = Clock::now());
    core::Expected<void> redo (Clock::time_point now = Clock::now());

    bool hasPendingWrite() const { return writeDeadline.has_value(); }

    // Writes song.json if a change is pending and its deadline has passed. Call it from a timer.
    // Errors: song.write_failed, song.json_unserializable; the write stays pending.
    core::Expected<void> writeIfDue (Clock::time_point now);

    // Writes song.json now if a change is pending. Errors: as writeIfDue.
    core::Expected<void> flush();

    // Captures the live plugins' state into a Song's Insert Chain slots (stateHash), storing the
    // blobs through storePluginState (plugin::InsertHost::captureStates, issue #33). The package
    // calls it on the Song of every history step (edit, and the current Song before an undo or
    // redo) and on the current Song before each debounced or flushed song.json write, so the
    // plugin state saved always matches what was heard. A capture that fails logs and still lets
    // the change through. Empty (the default): nothing is captured. The capture must not outlive
    // what it reads: clear it before destroying that.
    using PluginStateCapture = std::function<void (Song& song)>;
    void setPluginStateCapture (PluginStateCapture capture) { pluginStateCapture = std::move (capture); }

    // Stores plugin state as plugins/<sha256>.state (once per distinct content) and returns the
    // hash to put in an InsertSlot. Errors: song.write_failed.
    core::Expected<std::string> storePluginState (const juce::MemoryBlock& state);
    // Errors: song.plugin_state_invalid_hash, song.plugin_state_missing.
    core::Expected<juce::MemoryBlock> loadPluginState (const std::string& hash) const;

    // Imports an audio file as a Backing Track at the Song's rate (ADR 0004, issue #14 amendment):
    // writes its files (BackingTrackImport.h), then adds the Backing Track, a Track named after the
    // file and one Clip of it at sample 0 as one undo step "Import <file name>". Returns the
    // Backing Track id and logs song.backing_track_imported.
    // When decoding, a media write (original copy, derived WAV) or the history write fails, the
    // Song is unchanged, no undo step is added and every file the import wrote is deleted.
    // When only the song.json write fails, the step is already committed: the import stands (Song
    // changed, files kept, song.backing_track_imported logged, the write pending) and that Error is
    // returned. A failed peaks write is only logged (song.peaks_write_failed): the cache is rebuilt.
    // Errors: song.import_unreadable, song.import_too_many_channels, song.write_failed,
    // song.json_unserializable, or the beforeRename hook's.
    core::Expected<Id> importBackingTrack (const juce::File& source, Clock::time_point now = Clock::now());

    // Several files: one importBackingTrack each, in order, so each becomes its own Track at sample
    // 0 and its own undo step. A failed file does not stop the others.
    std::vector<core::Expected<Id>> importBackingTracks (const std::vector<juce::File>& sources, Clock::time_point now = Clock::now());

    // Adds a Separation's parts (issue #114, song/Separation.h): each of `partFiles` (one per
    // separationParts entry, in that order: the helper's WAVs) becomes a Backing Track the way an
    // import does (its original copy, a derived WAV at the Song's rate, peaks) that remembers the
    // source Track's Backing Track and `model` (SeparationOrigin); then withSeparation as one undo
    // step "Separate <source's original filename>". Returns the new Backing Track ids in part order.
    // As with an import: until the history step is committed any failure leaves the Song unchanged
    // and deletes every file this wrote; once committed, a failed song.json write is returned but
    // the Separation stands. The files are never deleted by undo, so redo plays.
    // Errors: separableBackingTrack's, song.separation_parts_invalid, writeBackingTrackFiles's,
    // edit()'s.
    core::Expected<std::vector<Id>> addSeparation (const Id& sourceTrackId, const std::vector<juce::File>& partFiles, const std::string& model, Clock::time_point now = Clock::now());

    // True when the Backing Track's derived file is not there to play (shown as missing).
    bool isBackingTrackMissing (const Id& backingTrackId) const;

    // The peaks of a Take's or Backing Track's media, from the cache, rebuilt (song.peaks_rebuilt)
    // when missing or corrupt. Errors: song.media_unknown (source_kind, source_id: not in the Song),
    // song.read_failed (the media file is missing), song.write_failed.
    // Not const: a rebuild writes peaks/<media-id>.peaks and logs.
    core::Expected<Peaks> peaks (const ClipSource& source);

    // Duplicate Song: flushes, then clonefile()s the whole package, history included, ids kept.
    // `destination` must not exist. Errors: song.duplicate_failed (path, destination, errno), or
    // the flush's.
    core::Expected<void> duplicateTo (const juce::File& destination);

private:
    SongPackage (juce::File root, Song song, SongHistory history, PackageOptions options);

    // What an undoable change did. committed: the history step was written and the change applied.
    // result: when committed, the song.json write's result; otherwise the history write's Error.
    struct EditOutcome
    {
        bool committed = false;
        core::Expected<void> result;
    };

    // edit(), reporting whether the step was committed (importBackingTrack's file cleanup needs it).
    EditOutcome commitEdit (Song changed, std::string label, Clock::time_point now);

    core::Expected<void> restoreSnapshot (core::Expected<Song> snapshot, Clock::time_point now);
    core::Expected<void> writeCommittedStep (Clock::time_point now);
    void scheduleWrite (Clock::time_point now);
    core::Expected<void> writeSongJson();
    void capturePluginState (Song& song);

    juce::File root;
    Song currentSong;
    SongHistory history;
    PackageOptions options;
    std::optional<Clock::time_point> writeDeadline;
    PluginStateCapture pluginStateCapture;
};

struct SongPackage::Opened
{
    std::unique_ptr<SongPackage> package;
    std::vector<Notice> notices; // e.g. history dropped; empty when all is well
};
} // namespace overdub::song
