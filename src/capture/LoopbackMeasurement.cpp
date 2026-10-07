#include "capture/LoopbackMeasurement.h"

#include <algorithm>
#include <cmath>

namespace overdub::capture
{
namespace
{
constexpr double burstSeconds = 0.010;
constexpr double signalSeconds = 6.0;
constexpr double burstTimesSeconds[] = { 0.5, 1.3, 2.1, 2.9, 3.6, 4.4, 5.2 };
constexpr float burstAmplitude = 0.5f;
constexpr int noiseSeed = 42;
constexpr double signalFoundCorrelation = 0.2;

struct BurstMatch
{
    SampleCount lag = 0;
    double normalisedCorrelation = 0.0;
};

// The lag in [-maxLag, maxLag] where the recorded audio best matches this burst (largest
// |correlation|: a loopback may invert polarity).
BurstMatch matchBurst (const float* burst, int burstLength, SampleCount burstSongSample, const float* recorded, SampleCount recordedLength, SampleCount recordedOrigin, int maxLag)
{
    double burstEnergy = 0.0;
    for (int i = 0; i < burstLength; ++i)
        burstEnergy += (double) burst[i] * burst[i];

    BurstMatch best;
    double bestValue = -1.0;
    double bestEnergy = 0.0;
    for (SampleCount lag = -maxLag; lag <= maxLag; ++lag)
    {
        const auto offset = burstSongSample + lag - recordedOrigin; // recorded index of burst[0]
        if (offset < 0 || offset + burstLength > recordedLength)
            continue;

        double sum = 0.0;
        double energy = 0.0;
        for (int i = 0; i < burstLength; ++i)
        {
            const double sample = recorded[offset + i];
            sum += burst[i] * sample;
            energy += sample * sample;
        }
        if (std::abs (sum) > bestValue)
        {
            bestValue = std::abs (sum);
            best.lag = lag;
            bestEnergy = energy;
        }
    }

    if (bestValue > 0.0 && bestEnergy > 0.0 && burstEnergy > 0.0)
        best.normalisedCorrelation = bestValue / std::sqrt (bestEnergy * burstEnergy);
    return best;
}
} // namespace

LoopbackSignal makeLoopbackSignal (int sampleRate)
{
    LoopbackSignal signal;
    signal.audio.setSize (1, (int) (signalSeconds * sampleRate));
    signal.audio.clear();
    signal.burstLength = (int) (burstSeconds * sampleRate);

    juce::Random random { noiseSeed };
    for (const auto seconds : burstTimesSeconds)
    {
        const auto start = (SampleCount) (seconds * sampleRate);
        signal.burstStarts.push_back (start);
        for (int i = 0; i < signal.burstLength; ++i)
            signal.audio.setSample (0, (int) start + i, burstAmplitude * (random.nextFloat() * 2.0f - 1.0f));
    }
    return signal;
}

LoopbackMeasurement measureLoopback (const LoopbackSignal& played,
                                     SampleCount playedAt,
                                     const float* recorded,
                                     SampleCount recordedLength,
                                     SampleCount recordedOrigin,
                                     int maxLagSamples)
{
    LoopbackMeasurement measurement;
    for (const auto start : played.burstStarts)
    {
        const auto match = matchBurst (played.audio.getReadPointer (0, (int) start), played.burstLength, playedAt + start, recorded, recordedLength, recordedOrigin, maxLagSamples);
        measurement.lagsSamples.push_back (match.lag);
        measurement.peakCorrelation = std::max (measurement.peakCorrelation, match.normalisedCorrelation);
    }

    measurement.signalFound = measurement.peakCorrelation > signalFoundCorrelation && ! measurement.lagsSamples.empty();
    if (measurement.lagsSamples.empty())
        return measurement;

    auto sorted = measurement.lagsSamples;
    std::ranges::sort (sorted);
    measurement.offsetSamples = sorted[sorted.size() / 2];
    measurement.jitterSamples = sorted.back() - sorted.front();
    return measurement;
}

bool loopbackPassed (const LoopbackMeasurement& measurement)
{
    return measurement.signalFound && measurement.jitterSamples <= loopbackJitterLimitSamples;
}

core::Fields loopbackReportFields (const LoopbackMeasurement& measurement)
{
    return {
        { "pass", loopbackPassed (measurement) },
        { "signal_found", measurement.signalFound },
        { "peak_correlation", measurement.peakCorrelation },
        { "lags_samples", measurement.lagsSamples },
        { "offset_samples", measurement.offsetSamples },
        { "jitter_samples", measurement.jitterSamples },
        { "jitter_limit_samples", loopbackJitterLimitSamples },
    };
}
} // namespace overdub::capture
