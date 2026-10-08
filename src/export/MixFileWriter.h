#pragma once

#include "core/Error.h"
#include "export/ExportFormat.h"

#include <juce_core/juce_core.h>

#include <memory>

// Writes the rendered mix to a file through Core Audio's ExtAudioFile (issue #37): WAV as 24-bit
// integer PCM, M4A as AAC at 256 kbit/s, both at the Song's rate. The caller hands it float
// blocks, one pointer per channel; ExtAudioFile converts (and, for AAC, records the encoder
// priming so decoders trim it).
//
// Errors: export.write_failed (path, step: create | client_format | bit_rate | write | close,
// os_status), export.format_unavailable (format) for a format that is not
// an audio file (MP4 is export/MovieExport.h's).
namespace overdub::exporting
{
inline constexpr int aacBitRate = 256'000;

class MixFileWriter
{
public:
    static core::Expected<std::unique_ptr<MixFileWriter>> create (const juce::File& file, ExportFormat format, int sampleRate, int numChannels);
    // Disposes of the file if close() was not called (what it holds may be incomplete).
    ~MixFileWriter();

    MixFileWriter (const MixFileWriter&) = delete;
    MixFileWriter& operator= (const MixFileWriter&) = delete;

    core::Expected<void> write (const float* const* channels, int numSamples);
    // Flushes and closes the file (the AAC encoder's last packets are written here).
    core::Expected<void> close();

private:
    struct Handle;
    MixFileWriter (juce::File file, std::unique_ptr<Handle> handle, int numChannels);

    juce::File file;
    std::unique_ptr<Handle> handle;
    int numChannels = 0;
};
} // namespace overdub::exporting
