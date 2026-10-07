#pragma once

#include "core/Log.h"
#include "song/Song.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <vector>

// The loopback latency measurement (ADR 0002, ADR 0005): play noise bursts, record them back
// through a cable or speakers and microphone, and cross-correlate each burst to find where the
// recorded Take put it on the timeline. `overdub --selftest loopback` reports it as JSON; the
// calibration flow (slice 18) stores offsetSamples as the device's calibration offset.
// Pure functions: no I/O, no logging.
namespace overdub::capture
{
using song::SampleCount;

// ADR 0005: the loopback selftest passes when the bursts' lags spread by no more than this.
inline constexpr SampleCount loopbackJitterLimitSamples = 5;

// Mono noise bursts at fixed times. Deterministic: the same rate gives the same samples.
struct LoopbackSignal
{
    juce::AudioBuffer<float> audio; // one channel
    std::vector<SampleCount> burstStarts;
    int burstLength = 0;
};

// Seven 10 ms bursts of seeded noise (amplitude 0.5) at 0.5, 1.3, 2.1, 2.9, 3.6, 4.4 and 5.2 s,
// in 6 s of silence.
LoopbackSignal makeLoopbackSignal (int sampleRate);

struct LoopbackMeasurement
{
    // Per burst: recorded timeline position minus played position. > 0: the Take lands late.
    std::vector<SampleCount> lagsSamples;
    // The median lag: what to add to the calibration offset so Takes land where they were heard.
    SampleCount offsetSamples = 0;
    // max lag - min lag across the bursts.
    SampleCount jitterSamples = 0;
    // The best normalised correlation of any burst, 0..1: how clearly the bursts came back.
    double peakCorrelation = 0.0;
    // peakCorrelation > 0.2: the bursts were captured at all.
    bool signalFound = false;
};

// `played` was played with its sample 0 at Song sample `playedAt`; `recorded` (mono) holds a Take
// whose sample 0 belongs at Song sample `recordedOrigin`. Searches lags within ±maxLagSamples.
LoopbackMeasurement measureLoopback (const LoopbackSignal& played,
                                     SampleCount playedAt,
                                     const float* recorded,
                                     SampleCount recordedLength,
                                     SampleCount recordedOrigin,
                                     int maxLagSamples);

// Signal found and jitter within loopbackJitterLimitSamples.
bool loopbackPassed (const LoopbackMeasurement& measurement);

// The measurement's own fields of the selftest report: signal_found, peak_correlation,
// lags_samples, offset_samples, jitter_samples, jitter_limit_samples, pass.
core::Fields loopbackReportFields (const LoopbackMeasurement& measurement);
} // namespace overdub::capture
