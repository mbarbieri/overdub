#include "song/BackingTrackImport.h"

#include "song/AudioFileDecode.h"
#include "song/Peaks.h"

#include <cerrno>
#include <cstdio>

namespace overdub::song
{
juce::File derivedFileFor (const juce::File& packageRoot, const BackingTrack& backingTrack)
{
    return packageRoot.getChildFile ("backing").getChildFile (juce::String (backingTrack.id) + ".wav");
}

juce::File originalFileFor (const juce::File& packageRoot, const BackingTrack& backingTrack)
{
    return packageRoot.getChildFile ("backing").getChildFile (juce::String (backingTrack.id) + ".orig." + juce::String (backingTrack.originalExtension));
}

namespace
{
// Copies `source` to `<destination>.tmp`, then renames it.
core::Expected<void> copyFileAtomically (const juce::File& source, const juce::File& destination, const BeforeRenameHook& beforeRename)
{
    const auto tempFile = destination.getSiblingFile (destination.getFileName() + ".tmp");
    if (! source.copyFileTo (tempFile))
        return std::unexpected (writeFailed (tempFile, "copy", 0));
    if (auto synced = syncFile (tempFile); ! synced.has_value())
        return synced;
    if (beforeRename)
        if (auto injected = beforeRename (destination); injected.has_value())
            return std::unexpected (*injected);
    if (std::rename (tempFile.getFullPathName().toRawUTF8(), destination.getFullPathName().toRawUTF8()) != 0)
        return std::unexpected (writeFailed (destination, "rename", errno));
    return {};
}

core::Expected<void> rebuildDerivedFile (const juce::File& packageRoot, const BackingTrack& backingTrack, int sampleRate, const BeforeRenameHook& beforeRename)
{
    const auto decoded = decodeAudioFile (originalFileFor (packageRoot, backingTrack), sampleRate);
    if (! decoded.has_value())
        return std::unexpected (decoded.error());
    return writeFloatWavAtomically (derivedFileFor (packageRoot, backingTrack), decoded->audio, sampleRate, beforeRename);
}
} // namespace

core::Expected<BackingTrack> writeBackingTrackFiles (const juce::File& packageRoot, const juce::File& source, int sampleRate, const BeforeRenameHook& beforeRename)
{
    auto decoded = decodeAudioFile (source, sampleRate);
    if (! decoded.has_value())
        return std::unexpected (decoded.error());

    BackingTrack backingTrack;
    backingTrack.id = makeId();
    backingTrack.originalFilename = source.getFileName().toStdString();
    backingTrack.originalExtension = source.getFileExtension().trimCharactersAtStart (".").toStdString();
    backingTrack.originalSampleRate = decoded->originalSampleRate;
    backingTrack.channels = decoded->channels;
    backingTrack.formatName = decoded->formatName;
    backingTrack.lengthSamples = decoded->audio.getNumSamples();

    const auto backingFolder = packageRoot.getChildFile ("backing");
    if (const auto created = backingFolder.createDirectory(); created.failed())
        return std::unexpected (writeFailed (backingFolder, "create_folder", 0));

    if (auto copied = copyFileAtomically (source, originalFileFor (packageRoot, backingTrack), beforeRename); ! copied.has_value())
    {
        deleteBackingTrackFiles (packageRoot, backingTrack);
        return std::unexpected (copied.error());
    }
    if (auto written = writeFloatWavAtomically (derivedFileFor (packageRoot, backingTrack), decoded->audio, sampleRate, beforeRename); ! written.has_value())
    {
        deleteBackingTrackFiles (packageRoot, backingTrack);
        return std::unexpected (written.error());
    }
    if (auto peaks = writePeaksFile (peaksFileFor (packageRoot, backingTrack.id), decoded->audio, beforeRename); ! peaks.has_value())
        core::logError (core::Error { "song.peaks_write_failed", "could not write the peaks cache", peaks.error().fields }, core::Level::warn);
    return backingTrack;
}

void deleteBackingTrackFiles (const juce::File& packageRoot, const BackingTrack& backingTrack)
{
    for (const auto& file : { originalFileFor (packageRoot, backingTrack), derivedFileFor (packageRoot, backingTrack), peaksFileFor (packageRoot, backingTrack.id) })
    {
        file.deleteFile();
        file.getSiblingFile (file.getFileName() + ".tmp").deleteFile();
    }
}

Song withImportedBackingTrack (Song song, const BackingTrack& backingTrack)
{
    Clip clip;
    clip.id = makeId();
    clip.source = ClipSource { ClipSourceKind::backing, backingTrack.id };
    clip.timelineStart = 0;
    clip.sourceOffset = 0;
    clip.length = backingTrack.lengthSamples;

    Track track;
    track.id = makeId();
    track.name = juce::String (backingTrack.originalFilename).upToLastOccurrenceOf (".", false, false).toStdString();
    track.clips.push_back (std::move (clip));

    song.backingTracks.push_back (backingTrack);
    song.tracks.push_back (std::move (track));
    return song;
}

std::string importStepLabel (const BackingTrack& backingTrack)
{
    return "Import " + backingTrack.originalFilename;
}

std::set<Id> repairBackingTrackFiles (const juce::File& packageRoot, const Song& song, const BeforeRenameHook& beforeRename)
{
    std::set<Id> missing;
    for (const auto& backingTrack : song.backingTracks)
    {
        const auto derived = derivedFileFor (packageRoot, backingTrack);
        if (derived.existsAsFile())
            continue;

        const auto original = originalFileFor (packageRoot, backingTrack);
        const auto rebuilt = original.existsAsFile() ? rebuildDerivedFile (packageRoot, backingTrack, song.sampleRate, beforeRename)
                                                     : core::Expected<void> (std::unexpected (core::Error { "song.read_failed", "the original file is missing", { { "path", pathOf (original) } } }));
        if (rebuilt.has_value())
        {
            core::logEvent (core::Level::info, "song.backing_track_rebuilt", { { "backing_track_id", backingTrack.id }, { "path", pathOf (derived) }, { "original", pathOf (original) } });
            continue;
        }

        core::logEvent (core::Level::warn,
                        "song.backing_track_missing",
                        { { "backing_track_id", backingTrack.id }, { "path", pathOf (derived) }, { "original", pathOf (original) }, { "cause", rebuilt.error().code } },
                        rebuilt.error().message);
        missing.insert (backingTrack.id);
    }
    return missing;
}
} // namespace overdub::song
