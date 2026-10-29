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

// What follows the Backing Track's id in the name of its render at `transpose` (not none).
juce::String renderSuffix (Transpose transpose)
{
    return juce::String (renderInfix) + juce::String (transposeFileTag (transpose)) + ".wav";
}

// "+3", "-2", "+0": a sign always, ASCII minus.
std::string fileSigned (int value)
{
    return (value >= 0 ? "+" : "") + std::to_string (value);
}
} // namespace

core::Expected<Song> setTranspose (Song song, Transpose transpose)
{
    if (transpose.semitones < minTranspose || transpose.semitones > maxTranspose)
        return std::unexpected (core::Error {
            "song.transpose_out_of_range",
            "Transpose is a whole number of semitones from -12 to +12",
            { { "semitones", transpose.semitones }, { "min", minTranspose }, { "max", maxTranspose } },
        });
    if (transpose.cents < minTransposeCents || transpose.cents > maxTransposeCents)
        return std::unexpected (core::Error {
            "song.transpose_cents_out_of_range",
            "the Transpose fine tune is a whole number of cents from -50 to +50",
            { { "cents", transpose.cents }, { "min", minTransposeCents }, { "max", maxTransposeCents } },
        });
    song.transpose = transpose;
    return song;
}

double transposeSemitones (Transpose transpose)
{
    return transpose.semitones + transpose.cents / 100.0;
}

std::string signedText (int value)
{
    if (value < 0)
        return "\xe2\x88\x92" + std::to_string (-value);
    return (value > 0 ? "+" : "") + std::to_string (value);
}

std::string transposeText (Transpose transpose)
{
    if (transpose.cents == 0)
        return signedText (transpose.semitones);
    return signedText (transpose.semitones) + " " + signedText (transpose.cents) + " ct";
}

std::string transposeLabel (Transpose transpose)
{
    return "Transpose " + transposeText (transpose);
}

std::string transposeFileTag (Transpose transpose)
{
    if (transpose.cents == 0)
        return fileSigned (transpose.semitones);
    return fileSigned (transpose.semitones) + fileSigned (transpose.cents) + "c";
}

juce::File transposedFileFor (const juce::File& packageRoot, const BackingTrack& backingTrack, Transpose transpose)
{
    if (transpose.isNone())
        return derivedFileFor (packageRoot, backingTrack);
    return packageRoot.getChildFile (backingFolderName).getChildFile (juce::String (backingTrack.id) + renderSuffix (transpose));
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
    if (song.transpose.isNone())
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

int deleteTransposeRendersExcept (const juce::File& packageRoot, Transpose kept)
{
    const auto keptSuffix = renderSuffix (kept);
    int deleted = 0;
    for (const auto& file : packageRoot.getChildFile (backingFolderName).findChildFiles (juce::File::findFiles, false, juce::String ("*") + renderInfix + "*.wav"))
        if (kept.isNone() || ! file.getFileName().endsWith (keptSuffix))
            if (file.deleteFile())
                ++deleted;
    return deleted;
}

int deleteTransposeRendersOfRemovedBackingTracks (const juce::File& packageRoot, const Song& song)
{
    int deleted = 0;
    for (const auto& file : packageRoot.getChildFile (backingFolderName).findChildFiles (juce::File::findFiles, false, juce::String ("*") + renderInfix + "*.wav"))
    {
        const auto backingTrackId = file.getFileName().upToFirstOccurrenceOf (renderInfix, false, false).toStdString();
        if (! std::ranges::contains (song.backingTracks, backingTrackId, &BackingTrack::id))
            if (file.deleteFile())
                ++deleted;
    }
    return deleted;
}
} // namespace overdub::song
