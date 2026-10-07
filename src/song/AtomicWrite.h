#pragma once

#include "core/Error.h"

#include <juce_core/juce_core.h>

#include <functional>
#include <optional>
#include <string>
#include <string_view>

// Atomic file replacement for the package (ADR 0004): write `<destination>.tmp`, fsync, rename.
// A reader sees either the old file or the new one, never a partial write. Leftover *.tmp files
// are crash debris; SongPackage::open deletes them.
namespace overdub::song
{
// Test seam: called after the temp file is complete, before the rename. An Error aborts the
// write and leaves the temp file in place, as a crash at that moment would.
using BeforeRenameHook = std::function<std::optional<core::Error> (const juce::File& destination)>;

// Errors: song.write_failed (path, errno, step: open|write|fsync|rename), or the hook's Error.
core::Expected<void> writeFileAtomically (const juce::File& destination, std::string_view bytes, const BeforeRenameHook& beforeRename);

// Errors: song.read_failed (path).
core::Expected<std::string> readTextFile (const juce::File& file);

// Deletes every *.tmp file under `directory`, recursively. Returns how many were deleted.
int deleteTempFiles (const juce::File& directory);
} // namespace overdub::song
