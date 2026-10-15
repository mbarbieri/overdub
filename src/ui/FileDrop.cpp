#include "ui/FileDrop.h"

#include <algorithm>
#include <array>

namespace overdub::ui
{
namespace
{
// Lower case, without the dot: the formats song::decodeAudioFile reads.
constexpr std::array<const char*, 8> importableExtensions { "wav", "aif", "aiff", "aifc", "caf", "m4a", "mp3", "flac" };

bool hasImportableExtension (const juce::File& file)
{
    const auto extension = file.getFileExtension().trimCharactersAtStart (".").toLowerCase();
    return std::ranges::any_of (importableExtensions, [&extension] (const char* importable) { return extension == importable; });
}
} // namespace

juce::String importableAudioPattern()
{
    juce::StringArray patterns;
    for (const auto* extension : importableExtensions)
        patterns.add (juce::String ("*.") + extension);
    return patterns.joinIntoString (";");
}

std::vector<juce::File> importableAudioFiles (const juce::StringArray& paths)
{
    std::vector<juce::File> files;
    for (const auto& path : paths)
    {
        const juce::File file (path);
        if (! file.isDirectory() && hasImportableExtension (file))
            files.push_back (file);
    }
    return files;
}
} // namespace overdub::ui
