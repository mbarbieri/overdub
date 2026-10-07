#pragma once

#include "core/Error.h"
#include "song/AtomicWrite.h"
#include "song/Song.h"
#include "song/SongHistory.h"

#include <juce_core/juce_core.h>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// A Song on disk: the always-saved `.overdub` package with persisted undo history (ADR 0004).
//
//   <name>.overdub/
//     song.json                  the current Song, written debounced and atomically (temp + rename)
//     history/NNNNNN.json        { "label", "song" }: one whole-Song snapshot per step (SongHistory.h)
//     history/index.json         { "oldest", "newest", "current" }
//     plugins/<sha256>.state     plugin state blobs, content-addressed, written once
//     audio/ backing/ video/     media, written by later slices; never touched here
//
// There is no Save. Two change paths: edit() adds an undo step; changeWithoutHistory() does not
// (Armed, input channels, plugin parameters, pre-roll, camera, playhead, export choice). Both
// schedule the song.json write; history files and index.json are written at once.
//
// Single-threaded: every call comes from one thread (the message thread in the app).
namespace overdub::song
{
// A one-line message for the user, surfaced by the UI (e.g. "Undo history could not be read").
struct Notice
{
    std::string code; // same namespace as events, e.g. "song.history_dropped"
    std::string message;
};

struct PackageOptions
{
    // The song.json write happens at most this long after the first unsaved change. The deadline
    // is not pushed back by later changes, so a stream of changes (playhead) still gets written.
    std::chrono::milliseconds saveDelay { 500 };

    // Test seam: called with the destination after its temp file is written, before the rename.
    // Returning an Error aborts that write and leaves the temp file behind, as a crash would.
    BeforeRenameHook beforeRename;
};

class SongPackage
{
public:
    using Clock = std::chrono::steady_clock;

    struct Opened;

    // Creates a new package at `location`, which must not exist yet. Errors: song.package_exists,
    // song.create_failed, plus any write error.
    static core::Expected<std::unique_ptr<SongPackage>> create (const juce::File& location, Song song, PackageOptions options = {});

    // Opens an existing package: deletes leftover *.tmp files, migrates song.json and every
    // snapshot, drops unreadable history (a Notice, never an Error), deletes unreferenced plugin
    // blobs. Errors: song.package_not_found, song.read_failed, song.json_unparsable,
    // song.json_invalid, song.format_too_new, song.migration_failed.
    static core::Expected<Opened> open (const juce::File& location, PackageOptions options = {});

    // Writes any pending change; a failure is logged.
    ~SongPackage();

    SongPackage (const SongPackage&) = delete;
    SongPackage& operator= (const SongPackage&) = delete;

    const Song& song() const { return currentSong; }
    const juce::File& location() const { return root; }

    // An undoable change: one history step named `label` ("Import Band Mix.mp3").
    core::Expected<void> edit (Song changed, std::string label, Clock::time_point now = Clock::now());

    // A change that is saved but not undoable.
    void changeWithoutHistory (Song changed, Clock::time_point now = Clock::now());

    bool canUndo() const { return history.canUndo(); }
    bool canRedo() const { return history.canRedo(); }
    std::string undoLabel() const { return history.undoLabel(); }
    std::string redoLabel() const { return history.redoLabel(); }

    // Restore the neighbouring snapshot, keeping state outside history (keepStateOutsideHistory).
    // Errors: song.history_empty (nothing to undo/redo), plus index.json write errors.
    core::Expected<void> undo (Clock::time_point now = Clock::now());
    core::Expected<void> redo (Clock::time_point now = Clock::now());

    bool hasPendingWrite() const { return writeDeadline.has_value(); }

    // Writes song.json if a change is pending and its deadline has passed. Call it from a timer.
    core::Expected<void> writeIfDue (Clock::time_point now);

    // Writes song.json now if a change is pending.
    core::Expected<void> flush();

    // Stores plugin state as plugins/<sha256>.state (once per distinct content) and returns the
    // hash to put in an InsertSlot.
    core::Expected<std::string> storePluginState (const juce::MemoryBlock& state);
    core::Expected<juce::MemoryBlock> loadPluginState (const std::string& hash) const;

    // Duplicate Song: flushes, then clonefile()s the whole package, history included, ids kept.
    // `destination` must not exist. Errors: song.duplicate_failed (errno).
    core::Expected<void> duplicateTo (const juce::File& destination);

private:
    SongPackage (juce::File root, Song song, SongHistory history, PackageOptions options);

    void scheduleWrite (Clock::time_point now);
    core::Expected<void> writeSongJson();

    juce::File root;
    Song currentSong;
    SongHistory history;
    PackageOptions options;
    std::optional<Clock::time_point> writeDeadline;
};

struct SongPackage::Opened
{
    std::unique_ptr<SongPackage> package;
    std::vector<Notice> notices; // e.g. history dropped; empty when all is well
};
} // namespace overdub::song
