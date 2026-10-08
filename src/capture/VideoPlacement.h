#pragma once

#include "engine/Anchor.h"
#include "song/Song.h"

#include <cstdint>
#include <optional>
#include <vector>

// Video Take placement on the audio clock (ADR 0003). Pure functions, no AVFoundation.
//
// A Pass's anchors (one per callback: the Song sample at the block's first output sample and that
// sample's output IO host time) are fitted by least squares to the line
//     hostNs (s) = originHostNs + hostOffsetNs + nsPerSample * (s - originSample)
// nsPerSample carries the audio clock's drift against host time. The sound of Song sample s is
// heard at hostNs (s) + output latency, so the Song sample heard at host time H is
//     (H - hostNs (originSample)) / nsPerSample + originSample - outputLatencySamples.
// A Video Take starts at the Song sample heard at its first frame's host time, minus the camera
// calibration (a camera that stamps frames late captured them earlier). Movie time t then plays at
//     startSample + t * songSamplesPerMovieSecond
// Movie time is each frame's own timestamp: nothing here counts frames, so a camera stall moves
// nothing.
namespace overdub::capture
{
using song::SampleCount;

struct AnchorFit
{
    // The first anchor, kept exact so the line is evaluated near it in double precision.
    SampleCount originSample = 0;
    std::uint64_t originHostNs = 0;
    double hostOffsetNs = 0.0; // the line's host time at originSample, relative to originHostNs
    double nsPerSample = 0.0;
    int anchorCount = 0;
    double maxResidualNs = 0.0;

    // The audio clock's rate against host time, in ppm: positive when the device runs fast.
    double driftPpm (int sampleRate) const;
};

// Least-squares fit of the anchors. With one anchor the line runs at the nominal rate.
// None without anchors.
std::optional<AnchorFit> fitAnchors (const std::vector<engine::Anchor>& anchors, int sampleRate);

// The Song sample heard at this host time (fractional), given the device's output latency.
double songSampleHeardAt (const AnchorFit& fit, std::uint64_t hostNs, int outputLatencySamples);

struct VideoPlacement
{
    SampleCount startSample = 0;            // the Song sample at movie time 0, rounded to the nearest
    double songSamplesPerMovieSecond = 0.0; // the drift ratio (song::VideoTake)
    double driftPpm = 0.0;                  // the same ratio against the Song's rate, for logs
};

// Places a Video Take whose first frame was captured at `firstFrameHostNs` (its PTS converted to
// host time). calibrationOffsetSamples: the per-machine camera calibration (positive: the camera
// stamps frames late). captureClockRate: the capture clock's rate against host time
// (CMSyncGetRelativeRate; 1 when it is the host clock).
VideoPlacement placeVideoTake (const AnchorFit& fit,
                               std::uint64_t firstFrameHostNs,
                               int outputLatencySamples,
                               SampleCount calibrationOffsetSamples,
                               double captureClockRate,
                               int sampleRate);

// The Song sample (fractional) at which this movie time plays.
double songSampleAtMovieTime (SampleCount startSample, double songSamplesPerMovieSecond, double movieSeconds);
} // namespace overdub::capture
