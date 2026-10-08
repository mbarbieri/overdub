#pragma once

#include "capture/MovieFrames.h"
#include "core/Log.h"
#include "song/Song.h"

#include <optional>
#include <vector>

// The clap measurement (ADR 0005): how far a Video Take sits from its Pass's audio, measured on
// claps. `overdub --selftest clap <package>` reports it, and the camera calibration flow
// (slice 18) reuses it. Pure: the caller decodes the Take and reads the movie's frames.
//
// Audio: each clap is the first sample over 30% of the Take's peak, at least 250 ms after the
// previous clap. Video: around where the clap should appear, the frame-to-frame motion peaks
// while the hands close and falls when they meet; the hands met between the last frame of that
// burst and the frame before it, so the visual clap is the midpoint of those two frames (good to
// ±½ frame). The offset is visual minus audio, in ms: positive means the video is late.
namespace overdub::capture
{
using song::SampleCount;

// Clap onsets in the samples, as sample indices.
std::vector<SampleCount> findClaps (const std::vector<float>& samples, int sampleRate);

// The visual clap's movie time within ±windowSeconds of `aroundSeconds`; none when no frame there
// moved.
std::optional<double> visualClapSeconds (const std::vector<MovieFrame>& frames, double aroundSeconds, double windowSeconds);

// Where the Pass's audio and its Video Take sit on the Song.
struct ClapPlacement
{
    int sampleRate = 48000;
    SampleCount takeOrigin = 0;             // the Song sample of the Take's first sample
    SampleCount videoStartSample = 0;       // the Song sample at movie time 0
    double songSamplesPerMovieSecond = 0.0; // the Video Take's drift ratio
};

struct ClapResult
{
    SampleCount songSample = 0;  // where the clap is heard
    double audioMovieSeconds = 0.0; // the movie time that Song sample plays at
    std::optional<double> visualMovieSeconds;
    std::optional<double> offsetMs; // visual minus audio
};

struct ClapReport
{
    std::vector<ClapResult> claps;
    double frameIntervalMs = 0.0; // the movie's median frame interval
    std::optional<double> meanOffsetMs;
    // At least one clap was seen, and every clap seen is within ±½ frame.
    bool pass = false;
};

ClapReport measureClaps (const std::vector<float>& takeSamples, const ClapPlacement& placement, const std::vector<MovieFrame>& frames);

// The report as JSON fields: pass, frame_interval_ms, mean_offset_ms, claps [{ song_sample,
// audio_movie_seconds, visual_movie_seconds, offset_ms }].
core::Fields clapReportFields (const ClapReport& report);
} // namespace overdub::capture
