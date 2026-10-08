#include "export/ExportFormat.h"

namespace overdub::exporting
{
const char* formatName (ExportFormat format)
{
    switch (format)
    {
        case ExportFormat::wav:
            return "wav";
        case ExportFormat::m4a:
            return "m4a";
        case ExportFormat::mp4:
            return "mp4";
    }
    return "wav";
}

bool isAvailable (ExportFormat format)
{
    return format != ExportFormat::mp4;
}

juce::File withFormatExtension (const juce::File& file, ExportFormat format)
{
    return file.withFileExtension (formatName (format));
}

juce::File defaultExportFile (const juce::File& folder, const std::string& songName, ExportFormat format)
{
    const auto name = songName.empty() ? juce::String ("Untitled") : juce::String::fromUTF8 (songName.c_str());
    return withFormatExtension (folder.getChildFile (juce::File::createLegalFileName (name)), format);
}
} // namespace overdub::exporting
