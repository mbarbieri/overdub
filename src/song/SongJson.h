#pragma once

#include "core/Error.h"
#include "song/Song.h"

#include <nlohmann/json.hpp>

// The song.json form of a Song (issue #8, ADR 0004). Keys are camelCase; the document carries
// `formatVersion`. All conversion is explicit (no NLOHMANN_DEFINE_* macros) and lives in
// SongJson.cpp. Reading never throws: a malformed document becomes an Error `song.json_invalid`
// whose `json_path` field names the offending JSON path (e.g. "$.tracks[0].clips[2].gainDb").
namespace overdub::song
{
nlohmann::json songToJson (const Song& song);

// Reads a document at the current songFormatVersion. Migrate older documents first.
core::Expected<Song> songFromJson (const nlohmann::json& document);

// Parses JSON text. Error `song.json_unparsable` with the parser's byte offset.
core::Expected<nlohmann::json> parseJsonText (const std::string& text);
} // namespace overdub::song
