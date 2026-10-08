#pragma once

#include "core/Log.h"

#include <juce_core/juce_core.h>

// `overdub --selftest clap <package>` (ADR 0005): measures, on claps, how far the newest Video
// Take of a Song sits from its Pass's audio. A human records a Pass with the camera armed while
// clapping in frame, then runs it on the Song's .overdub package. It reads the package without
// changing it: song.json, the Pass's first Take and the Video Take's movie
// (capture/ClapMeasurement.h does the measuring).
namespace overdub::capture
{
// The report: selftest "clap", pass, package, pass_id, take_id, video_take_id, camera_id,
// calibration_offset_samples, then clapReportFields; or pass false with `error` (an Error code:
// song.read_failed and the other song.json read errors, capture.clap_no_video_take,
// capture.clap_no_take, capture.movie_unreadable, the Take's WAV read errors) and `error_fields`.
core::Fields runClapSelftest (const juce::File& package);
} // namespace overdub::capture
