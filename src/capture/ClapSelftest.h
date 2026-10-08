#pragma once

#include "capture/ClapMeasurement.h"
#include "core/Error.h"
#include "core/Log.h"
#include "song/Song.h"

#include <juce_core/juce_core.h>

#include <string>

// `overdub --selftest clap <package>` (ADR 0005): measures, on claps, how far the newest Video
// Take of a Song sits from its Pass's audio. A human records a Pass with the camera armed while
// clapping in frame, then runs it on the Song's .overdub package. It reads the package without
// changing it: song.json, the Pass's first Take and the Video Take's movie
// (capture/ClapMeasurement.h does the measuring). The calibration flow (issue #39) measures the
// open Song the same way, through measureNewestVideoTake.
namespace overdub::capture
{
// The newest Video Take's clap measurement, with what the camera calibration needs.
struct VideoTakeClaps
{
    ClapReport report;
    song::Id passId;
    song::Id takeId;
    song::Id videoTakeId;
    std::string cameraId;
    SampleCount appliedCalibrationSamples = 0; // the camera calibration the Video Take was placed with
    int sampleRate = 0;                        // the Song's
    std::size_t frames = 0;
};

// Measures the claps of the newest Pass with a Video Take in `song`, whose media are in the package
// at `packageRoot`. Errors: capture.clap_no_video_take, capture.clap_no_take (pass_id),
// capture.movie_unreadable, the Take's WAV read Errors.
core::Expected<VideoTakeClaps> measureNewestVideoTake (const song::Song& song, const juce::File& packageRoot);

// The report: selftest "clap", pass, package, pass_id, take_id, video_take_id, camera_id,
// calibration_offset_samples, then clapReportFields; or pass false with `error` (an Error code:
// song.read_failed and the other song.json read errors, or measureNewestVideoTake's) and
// `error_fields`.
core::Fields runClapSelftest (const juce::File& package);
} // namespace overdub::capture
