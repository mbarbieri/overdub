#pragma once

#include <catch2/catch_test_macros.hpp>

// Measurements of synthetic stimuli shared by the song tests (ADR 0005).
namespace overdub::song::testing
{
// Frequency from the interpolated positive-going zero crossings in [from, to).
inline double measuredFrequency (const float* samples, int from, int to, int sampleRate)
{
    double first = -1.0;
    double last = -1.0;
    int crossings = 0;
    for (int i = from; i + 1 < to; ++i)
    {
        if (samples[i] < 0.0f && samples[i + 1] >= 0.0f)
        {
            const double at = i + (double) -samples[i] / (double) (samples[i + 1] - samples[i]);
            if (first < 0.0)
                first = at;
            last = at;
            ++crossings;
        }
    }
    REQUIRE (crossings > 2);
    return (crossings - 1) / (last - first) * sampleRate;
}
} // namespace overdub::song::testing
