#include "capture/ClapSelftest.h"

#include "capture/ClapMeasurement.h"
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
    core::Fields report { { "selftest", "clap" }, { "pass", false }, { "package", package.getFullPathName().toStdString() }, { "error", error.code } };
    for (const auto& [key, value] : error.fields.items())
        report[key] = value;
    return report;
}

core::Expected<song::Song> readSong (const juce::File& package)
{
    const auto songFile = package.getChildFile ("song.json");
    if (! songFile.existsAsFile())
        return std::unexpected (core::Error { "song.package_not_found", "no Song package at this location", { { "path", package.getFullPathName().toStdString() } } });
    auto stored = song::readStoredSong (songFile, "");
    if (! stored.has_value())
        return std::unexpected (stored.error());
    return std::move (stored->song);
}
} // namespace

core::Fields runClapSelftest (const juce::File& package)
{
    const auto song = readSong (package);
    if (! song.has_value())
        return failed (package, song.error());

    // The newest Pass with a Video Take.
    const song::VideoTake* videoTake = nullptr;
    for (const auto& pass : std::views::reverse (song->passes))
    {
        const auto found = std::ranges::find (song->videoTakes, pass.id, &song::VideoTake::passId);
        if (found != song->videoTakes.end())
        {
            videoTake = &*found;
            break;
        }
    }
    if (videoTake == nullptr)
        return failed (package, core::Error { "capture.clap_no_video_take", "the Song has no Pass with a Video Take", {} });

    const auto take = std::ranges::find (song->takes, videoTake->passId, &song::Take::passId);
    if (take == song->takes.end())
        return failed (package, core::Error { "capture.clap_no_take", "the Video Take's Pass has no Take", { { "pass_id", videoTake->passId } } });

    const auto audio = song::readWavFile (package.getChildFile (take->file));
    if (! audio.has_value())
        return failed (package, audio.error());
    const auto frames = readMovieFrames (package.getChildFile (videoTake->file));
    if (! frames.has_value())
        return failed (package, frames.error());

    const auto* channel = audio->getReadPointer (0);
    const std::vector<float> samples (channel, channel + audio->getNumSamples());
    const ClapPlacement placement {
        .sampleRate = song->sampleRate,
        .takeOrigin = take->origin,
        .videoStartSample = videoTake->startSample,
        .songSamplesPerMovieSecond = videoTake->songSamplesPerMovieSecond,
    };
    auto report = clapReportFields (measureClaps (samples, placement, *frames));
    report["selftest"] = "clap";
    report["package"] = package.getFullPathName().toStdString();
    report["pass_id"] = videoTake->passId;
    report["take_id"] = take->id;
    report["video_take_id"] = videoTake->id;
    report["camera_id"] = videoTake->cameraId;
    report["calibration_offset_samples"] = videoTake->calibrationOffsetSamples;
    report["frames"] = frames->size();
    return report;
}
} // namespace overdub::capture
