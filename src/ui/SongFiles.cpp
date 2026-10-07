#include "ui/SongFiles.h"

namespace overdub::ui
{
juce::File packageLocationFor (const juce::File& chosen)
{
    return chosen.hasFileExtension ("overdub") ? chosen : chosen.withFileExtension ("overdub");
}

juce::File packageLocationOpened (const juce::File& chosen)
{
    return chosen.existsAsFile() ? chosen.getParentDirectory() : chosen;
}

core::Expected<std::unique_ptr<song::SongPackage>> createSongAt (const juce::File& chosen, int deviceSampleRate)
{
    const auto sampleRate = deviceSampleRate > 0 ? deviceSampleRate : fallbackSongSampleRate;
    return song::SongPackage::create (packageLocationFor (chosen), song::makeEmptySong (sampleRate));
}

core::Expected<song::SongPackage::Opened> openSongAt (const juce::File& chosen)
{
    return song::SongPackage::open (packageLocationOpened (chosen));
}

core::Expected<juce::File> duplicateSongTo (song::SongPackage& package, const juce::File& chosen)
{
    const auto destination = packageLocationFor (chosen);
    if (auto duplicated = package.duplicateTo (destination); ! duplicated.has_value())
        return std::unexpected (duplicated.error());
    return destination;
}
} // namespace overdub::ui
