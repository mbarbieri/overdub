#include "capture/LoopbackMeasurement.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using overdub::capture::LoopbackSignal;
using overdub::capture::makeLoopbackSignal;
using overdub::capture::measureLoopback;
using overdub::song::SampleCount;

namespace
{
constexpr int sampleRate = 48000;
constexpr int maxLag = 2000;

// The signal as a Take would hold it: every burst moved by its own lag, the Take's sample 0 at
// Song sample `origin`, the signal played from Song sample `playedAt`, scaled by `gain`.
std::vector<float> recordedWithLags (const LoopbackSignal& signal, SampleCount playedAt, SampleCount origin, const std::vector<SampleCount>& lags, float gain = 1.0f)
{
    std::vector<float> recorded ((std::size_t) signal.audio.getNumSamples() + 2 * maxLag, 0.0f);
    for (std::size_t b = 0; b < signal.burstStarts.size(); ++b)
    {
        const auto start = signal.burstStarts[b];
        for (int i = 0; i < signal.burstLength; ++i)
            recorded[(std::size_t) (playedAt + start + lags[b] - origin + i)] = gain * signal.audio.getSample (0, (int) start + i);
    }
    return recorded;
}
} // namespace

TEST_CASE ("the loopback signal is seven 10 ms noise bursts in 6 s", "[capture][loopback]")
{
    const auto signal = makeLoopbackSignal (sampleRate);
    CHECK (signal.audio.getNumChannels() == 1);
    CHECK (signal.audio.getNumSamples() == 6 * sampleRate);
    CHECK (signal.burstLength == 480);
    CHECK (signal.burstStarts == std::vector<SampleCount> { 24000, 62400, 100800, 139200, 172800, 211200, 249600 });
    CHECK (signal.audio.getMagnitude (0, 0, 24000) == 0.0f);
    CHECK (signal.audio.getMagnitude (0, 24000, 480) > 0.4f);
}

TEST_CASE ("a Take that lands late by a constant offset measures that offset with no jitter", "[capture][loopback]")
{
    const auto signal = makeLoopbackSignal (sampleRate);
    const std::vector<SampleCount> lags (7, 821);
    // Inverted and quieter, as a speaker and microphone might return it.
    const auto recorded = recordedWithLags (signal, 1000, -300, lags, -0.3f);

    const auto measurement = measureLoopback (signal, 1000, recorded.data(), (SampleCount) recorded.size(), -300, maxLag);

    CHECK (measurement.lagsSamples == lags);
    CHECK (measurement.offsetSamples == 821);
    CHECK (measurement.jitterSamples == 0);
    CHECK (measurement.signalFound);
    CHECK (measurement.peakCorrelation > 0.99);
    CHECK (overdub::capture::loopbackPassed (measurement));
}

TEST_CASE ("bursts that land at different offsets report the median offset and their spread as jitter", "[capture][loopback]")
{
    const auto signal = makeLoopbackSignal (sampleRate);
    const std::vector<SampleCount> withinLimit { -12, -10, -11, -9, -14, -10, -13 };
    const std::vector<SampleCount> beyondLimit { -12, -10, -11, -9, -16, -10, -13 };

    const auto within = recordedWithLags (signal, 0, 0, withinLimit);
    const auto withinMeasurement = measureLoopback (signal, 0, within.data(), (SampleCount) within.size(), 0, maxLag);
    CHECK (withinMeasurement.lagsSamples == withinLimit);
    CHECK (withinMeasurement.offsetSamples == -11);
    CHECK (withinMeasurement.jitterSamples == 5);
    CHECK (overdub::capture::loopbackPassed (withinMeasurement));

    const auto beyond = recordedWithLags (signal, 0, 0, beyondLimit);
    const auto beyondMeasurement = measureLoopback (signal, 0, beyond.data(), (SampleCount) beyond.size(), 0, maxLag);
    CHECK (beyondMeasurement.jitterSamples == 7);
    CHECK_FALSE (overdub::capture::loopbackPassed (beyondMeasurement));

    const auto fields = overdub::capture::loopbackReportFields (beyondMeasurement);
    CHECK (fields["pass"] == false);
    CHECK (fields["jitter_samples"] == 7);
    CHECK (fields["jitter_limit_samples"] == 5);
    CHECK (fields["offset_samples"] == -11);
    CHECK (fields["lags_samples"] == beyondLimit);
}

TEST_CASE ("a silent recording finds no signal and fails", "[capture][loopback]")
{
    const auto signal = makeLoopbackSignal (sampleRate);
    const std::vector<float> silence ((std::size_t) signal.audio.getNumSamples(), 0.0f);

    const auto measurement = measureLoopback (signal, 0, silence.data(), (SampleCount) silence.size(), 0, maxLag);

    CHECK_FALSE (measurement.signalFound);
    CHECK_FALSE (overdub::capture::loopbackPassed (measurement));
    CHECK (overdub::capture::loopbackReportFields (measurement)["signal_found"] == false);
}
