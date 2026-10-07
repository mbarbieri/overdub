#pragma once

#include "core/Error.h"
#include "song/AtomicWrite.h"
#include "song/Song.h"

#include <juce_core/juce_core.h>

#include <set>
#include <string>

// Backing Track media in the package (ADR 0004, issue #14 amendment):
//   backing/<id>.orig.<ext>   the imported file, copied untouched
//   backing/<id>.wav          derived: 32-bit float at the Song's rate, what playback reads
//   peaks/<id>.peaks          the peaks cache (song/Peaks.h)
// SongPackage::importBackingTrack is the entry point; these are its file steps and the open-time
// repair. Single-threaded, like SongPackage.
namespace overdub::song
{
juce::File derivedFileFor (const juce::File& packageRoot, const BackingTrack& backingTrack);
juce::File originalFileFor (const juce::File& packageRoot, const BackingTrack& backingTrack);

// Decodes `source` at `sampleRate` and writes a new Backing Track's files: the original copy, the
// derived WAV (temp + rename) and its peaks. Returns the Backing Track to add to the Song. On an
// Error no file is left behind. Errors: decodeAudioFile's, song.write_failed, the hook's.
// A failed peaks write is only logged (song.peaks_write_failed, warn): it is a cache.
core::Expected<BackingTrack> writeBackingTrackFiles (const juce::File& packageRoot, const juce::File& source, int sampleRate, const BeforeRenameHook& beforeRename);

// Deletes the files writeBackingTrackFiles wrote, when the Song change that adds them fails.
void deleteBackingTrackFiles (const juce::File& packageRoot, const BackingTrack& backingTrack);

// The undoable content of an import: the Backing Track, a new Track named after the file (its name
// without the extension) holding one Clip of the whole file at sample 0, added after every Track.
Song withImportedBackingTrack (Song song, const BackingTrack& backingTrack);

// The undo step label of an import: "Import <original filename>".
std::string importStepLabel (const BackingTrack& backingTrack);

// Open-time repair of every Backing Track of `song`: a missing derived file is rebuilt from the
// original (song.backing_track_rebuilt, info). When it cannot be (both files missing, or the
// original unreadable), the Backing Track is missing: logged as song.backing_track_missing (warn:
// backing_track_id, path, cause) and its id returned. Its Clips play silence.
std::set<Id> repairBackingTrackFiles (const juce::File& packageRoot, const Song& song, const BeforeRenameHook& beforeRename);
} // namespace overdub::song
