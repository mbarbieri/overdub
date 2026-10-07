#pragma once

#include "core/Error.h"

#include <juce_core/juce_core.h>

#include <nlohmann/json.hpp>

#include <cstddef>
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

// The file's full path, for Error and event fields.
std::string pathOf (const juce::File& file);

// Errors: song.write_failed (path, step: open|write|fsync|rename, errno), or the hook's Error.
core::Expected<void> writeFileAtomically (const juce::File& destination, std::string_view bytes, const BeforeRenameHook& beforeRename);

// Serializes `document` (one space indent) and writes it with writeFileAtomically. Returns the
// number of bytes written. Errors: song.json_unserializable (path, reason; e.g. a string that is
// not valid UTF-8), plus writeFileAtomically's.
core::Expected<std::size_t> writeJsonAtomically (const juce::File& destination, const nlohmann::json& document, const BeforeRenameHook& beforeRename);

// Errors: song.read_failed (path).
core::Expected<std::string> readTextFile (const juce::File& file);

// Deletes every *.tmp file under `directory`, recursively. Returns how many were deleted.
int deleteTempFiles (const juce::File& directory);
} // namespace overdub::song
