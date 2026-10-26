#pragma once

#include "export/MovieSpan.h"
#include "song/Song.h"

#include <juce_core/juce_core.h>

#include <optional>
#include <string>

// What an Export writes (issues #37, #38): the format choice, its file extension and the request
// the Export sheet hands to the open Song. Pure; no I/O.
namespace overdub::exporting
{
using song::ExportFormat; // a Song setting (issue #201)

struct ExportRequest
{
    ExportFormat format = ExportFormat::wav;
    juce::File destination;
    // MP4 only: the Video Take shown, and the movie's length.
    std::optional<song::Id> videoTakeId = std::nullopt;
    VideoLength videoLength = VideoLength::trimToVideoTake;

    bool operator== (const ExportRequest&) const = default;
};

// "wav", "m4a", "mp4": the event field value and the file extension (without the dot).
const char* formatName (ExportFormat format);

// The file with its extension replaced by the format's ("Song.wav" -> "Song.m4a"); a file without
// an extension gains one.
juce::File withFormatExtension (const juce::File& file, ExportFormat format);

// Where an export goes until the user picks a place: <folder>/<song name>.<extension>.
juce::File defaultExportFile (const juce::File& folder, const std::string& songName, ExportFormat format);
} // namespace overdub::exporting
