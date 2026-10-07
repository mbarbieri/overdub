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

// The one shape of song.write_failed: path, step (what failed: open, write, copy, fsync, rename,
// clear_folder, create_folder), errno (0 when the failing call reports none) and, only when not
// empty, detail (the failing call's own description, for calls that report no errno).
core::Error writeFailed (const juce::File& file, const char* step, int savedErrno, const std::string& detail = {});

// `<destination>.tmp`: where every atomic writer puts the new content first.
juce::File tempFileFor (const juce::File& destination);

// The end of every atomic write, once `tempFile` holds the whole new content: fsyncs it, runs the
// hook, renames it over `destination`. Errors: song.write_failed (step fsync|rename), or the
// hook's Error (the temp file is then left in place).
core::Expected<void> renameIntoPlace (const juce::File& tempFile, const juce::File& destination, const BeforeRenameHook& beforeRename);

// Errors: song.write_failed (step open|write|fsync|rename), or the hook's Error.
core::Expected<void> writeFileAtomically (const juce::File& destination, std::string_view bytes, const BeforeRenameHook& beforeRename);

// Serializes `document` (one space indent) and writes it with writeFileAtomically. Returns the
// number of bytes written. Errors: song.json_unserializable (path, reason; e.g. a string that is
// not valid UTF-8), plus writeFileAtomically's.
core::Expected<std::size_t> writeJsonAtomically (const juce::File& destination, const nlohmann::json& document, const BeforeRenameHook& beforeRename);

// Errors: song.read_failed (path).
core::Expected<std::string> readTextFile (const juce::File& file);

// Reads and parses a JSON file. Errors: song.read_failed (path), song.json_unparsable (path,
// byte, reason).
core::Expected<nlohmann::json> readJsonFile (const juce::File& file);

// The Error with the file's path set as its `path` field, for Errors raised about its contents.
core::Error withFilePath (core::Error error, const juce::File& file);

// Deletes every *.tmp file under `directory`, recursively. Returns how many were deleted. A file
// that cannot be deleted is logged as song.temp_delete_failed (warn, path) and skipped.
int deleteTempFiles (const juce::File& directory);
} // namespace overdub::song
