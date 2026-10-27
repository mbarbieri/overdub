#include "song/Transpose.h"

#include "song/BackingTrackImport.h"
#include "song/PackageFolders.h"

#include <algorithm>

namespace overdub::song
{
namespace
{
// A render's name: <id>.transpose+3.wav. No other file in backing/ has it.
constexpr const char* renderInfix = ".transpose";

// What follows the Backing Track's id in the name of its render at `semitones` (not 0).
juce::String renderSuffix (int semitones)
{
    return juce::String (renderInfix) + (semitones > 0 ? "+" : "") + juce::String (semitones) + ".wav";
}
} // namespace

core::Expected<Song> setTranspose (Song song, int semitones)
{
    if (semitones < minTranspose || semitones > maxTranspose)
        return std::unexpected (core::Error {
            "song.transpose_out_of_range",
            "Transpose is a whole number of semitones from -12 to +12",
            { { "semitones", semitones }, { "min", minTranspose }, { "max", maxTranspose } },
        });
    song.transpose = semitones;
    return song;
}

std::string transposeText (int semitones)
{
    if (semitones < 0)
        return "\xe2\x88\x92" + std::to_string (-semitones);
    return (semitones > 0 ? "+" : "") + std::to_string (semitones);
}

std::string transposeLabel (int semitones)
{
    return "Transpose " + transposeText (semitones);
}

juce::File transposedFileFor (const juce::File& packageRoot, const BackingTrack& backingTrack, int semitones)
{
    if (semitones == 0)
        return derivedFileFor (packageRoot, backingTrack);
    return packageRoot.getChildFile (backingFolderName).getChildFile (juce::String (backingTrack.id) + renderSuffix (semitones));
}

std::optional<juce::File> playedFileFor (const juce::File& packageRoot, const Song& song, const ClipSource& source)
{
    if (source.kind == ClipSourceKind::take)
        return mediaFileFor (packageRoot, song, source);
    const auto backingTrack = std::ranges::find (song.backingTracks, source.id, &BackingTrack::id);
    if (backingTrack == song.backingTracks.end())
        return std::nullopt;
    return transposedFileFor (packageRoot, *backingTrack, song.transpose);
}

std::vector<TransposeRenderJob> missingTransposeRenders (const juce::File& packageRoot, const Song& song)
{
    std::vector<TransposeRenderJob> missing;
    if (song.transpose == 0)
        return missing;
    for (const auto& backingTrack : song.backingTracks)
    {
        const auto source = derivedFileFor (packageRoot, backingTrack);
        const auto destination = transposedFileFor (packageRoot, backingTrack, song.transpose);
        if (source.existsAsFile() && ! destination.existsAsFile())
            missing.push_back ({ backingTrack.id, source, destination });
    }
    return missing;
}

int deleteTransposeRendersExcept (const juce::File& packageRoot, int keptSemitones)
{
    const auto kept = renderSuffix (keptSemitones);
    int deleted = 0;
    for (const auto& file : packageRoot.getChildFile (backingFolderName).findChildFiles (juce::File::findFiles, false, juce::String ("*") + renderInfix + "*.wav"))
        if (keptSemitones == 0 || ! file.getFileName().endsWith (kept))
            if (file.deleteFile())
                ++deleted;
    return deleted;
}
} // namespace overdub::song
