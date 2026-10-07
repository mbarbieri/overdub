#pragma once

#include "core/Error.h"
#include "song/AtomicWrite.h"

#include <juce_core/juce_core.h>

#include <set>
#include <string>

// Content-addressed plugin state (ADR 0004): plugins/<sha256>.state, written once, never modified.
// An InsertSlot stores the hash, so history snapshots share blobs. Parameters are outside history:
// undo keeps a surviving slot's current hash (keepStateOutsideHistory in song/Song.h). The hash is the lowercase hex SHA-256 (CommonCrypto) of the state bytes.
namespace overdub::song
{
std::string sha256Hex (const juce::MemoryBlock& bytes);

// Writes the blob unless one with the same hash exists. Returns the hash.
core::Expected<std::string> storePluginBlob (const juce::File& pluginsDirectory, const juce::MemoryBlock& state, const BeforeRenameHook& beforeRename);

// Errors: song.plugin_state_missing (hash), song.plugin_state_invalid_hash (hash).
core::Expected<juce::MemoryBlock> loadPluginBlob (const juce::File& pluginsDirectory, const std::string& hash);

// Deletes every <hash>.state whose hash is not in `referenced`. Returns how many were deleted. A
// blob that cannot be deleted is logged as song.plugin_blob_delete_failed (warn, path).
int deleteUnreferencedPluginBlobs (const juce::File& pluginsDirectory, const std::set<std::string>& referenced);
} // namespace overdub::song
