#include "capture/ClapSelftest.h"

#include "capture/MovieFrames.h"
#include "song/AudioFileDecode.h"
#include "song/SongMigration.h"

#include <algorithm>
#include <ranges>

namespace overdub::capture
{
namespace
{
core::Fields failed (const juce::File& package, const core::Error& error)
{
    return { { "selftest", "clap" }, { "pass", false }, { "package", package.getFullPathName().toStdString() }, { "error", error.code }, { "error_fields", error.fields } };
}

// The package's Song through the one read path for package Songs (song/SongMigration.h).
core::Expected<song::Song> readSong (const juce::File& package)
{
    auto stored = song::readStoredSong (package.getChildFile ("song.json"), "");
    if (! stored.has_value())
        return std::unexpected (stored.error());
    return std::move (stored->song);
}

// The Video Take of the newest Pass that has one; null when none has.
const song::VideoTake* newestVideoTake (const song::Song& song)
{
    for (const auto& pass : std::views::reverse (song.passes))
    {
        const auto found = std::ranges::find (song.videoTakes, pass.id, &song::VideoTake::passId);
        if (found != song.videoTakes.end())
            return &*found;
    }
    return nullptr;
}
} // namespace

core::Expected<VideoTakeClaps> measureNewestVideoTake (const song::Song& song, const juce::File& packageRoot)
{
    const auto* videoTake = newestVideoTake (song);
    if (videoTake == nullptr)
        return std::unexpected (core::Error { "capture.clap_no_video_take", "the Song has no Pass with a Video Take", {} });

    const auto take = std::ranges::find (song.takes, videoTake->passId, &song::Take::passId);
    if (take == song.takes.end())
        return std::unexpected (core::Error { "capture.clap_no_take", "the Video Take's Pass has no Take", { { "pass_id", videoTake->passId } } });

    const auto audio = song::readWavFile (packageRoot.getChildFile (take->file));
    if (! audio.has_value())
        return std::unexpected (audio.error());
    const auto frames = readMovieFrames (packageRoot.getChildFile (videoTake->file));
    if (! frames.has_value())
        return std::unexpected (frames.error());

    const auto* channel = audio->getReadPointer (0);
    const std::vector<float> samples (channel, channel + audio->getNumSamples());
    const ClapPlacement placement {
        .sampleRate = song.sampleRate,
        .takeOrigin = take->origin,
        .videoStartSample = videoTake->startSample,
        .songSamplesPerMovieSecond = videoTake->songSamplesPerMovieSecond,
    };
    return VideoTakeClaps {
        .report = measureClaps (samples, placement, *frames),
        .passId = videoTake->passId,
        .takeId = take->id,
        .videoTakeId = videoTake->id,
        .cameraId = videoTake->cameraId,
        .appliedCalibrationSamples = videoTake->calibrationOffsetSamples,
        .sampleRate = song.sampleRate,
        .frames = frames->size(),
    };
}

core::Fields runClapSelftest (const juce::File& package)
{
    const auto song = readSong (package);
    if (! song.has_value())
        return failed (package, song.error());

    const auto measured = measureNewestVideoTake (*song, package);
    if (! measured.has_value())
        return failed (package, measured.error());

    auto report = clapReportFields (measured->report);
    report["selftest"] = "clap";
    report["package"] = package.getFullPathName().toStdString();
    report["pass_id"] = measured->passId;
    report["take_id"] = measured->takeId;
    report["video_take_id"] = measured->videoTakeId;
    report["camera_id"] = measured->cameraId;
    report["calibration_offset_samples"] = measured->appliedCalibrationSamples;
    report["frames"] = measured->frames;
    return report;
}
} // namespace overdub::capture
