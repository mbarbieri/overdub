#pragma once

#include "core/Error.h"

#include <juce_core/juce_core.h>

#include <string>

// What Export mode writes (issue #37): the format choice, its file extension and the request the
// Export page hands to the open Song. Pure; no I/O.
namespace overdub::exporting
{
enum class ExportFormat
{
    wav, // 24-bit integer PCM at the Song's rate
    m4a, // AAC at 256 kbit/s
    mp4  // the mix and one Video Take (slice 17): offered, not yet available
};

struct ExportRequest
{
    ExportFormat format = ExportFormat::wav;
    juce::File destination;

    bool operator== (const ExportRequest&) const = default;
};

// "wav", "m4a", "mp4": the event field value and the file extension (without the dot).
const char* formatName (ExportFormat format);

// Whether this build can write the format (mp4 arrives with slice 17).
bool isAvailable (ExportFormat format);
// The same as an Error: export.format_unavailable (format) when it cannot be written.
core::Expected<void> checkAvailable (ExportFormat format);

// The file with its extension replaced by the format's ("Song.wav" -> "Song.m4a"); a file without
// an extension gains one.
juce::File withFormatExtension (const juce::File& file, ExportFormat format);

// Where an export goes until the user picks a place: <folder>/<song name>.<extension>.
juce::File defaultExportFile (const juce::File& folder, const std::string& songName, ExportFormat format);
} // namespace overdub::exporting
