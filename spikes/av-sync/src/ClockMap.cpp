// PROTOTYPE — issue #7 spike.
#include "ClockMap.h"
#include <cmath>
#include <time.h>

ClockFit fitClock (const std::vector<ClockAnchor>& anchors, double sampleRate)
{
    ClockFit fit;
    fit.nominalNsPerSample = 1.0e9 / sampleRate;
    fit.nsPerSample = fit.nominalNsPerSample;
    fit.anchorCount = (int) anchors.size();

    if (anchors.empty())
        return fit;

    // Centre on the first anchor so doubles keep sub-nanosecond precision.
    const auto s0 = anchors.front().songSample;
    const auto h0 = anchors.front().hostNs;
    double sumX = 0, sumY = 0, sumXX = 0, sumXY = 0;

    for (auto& a : anchors)
    {
        const double x = (double) (a.songSample - s0);
        const double y = (double) (int64_t) (a.hostNs - h0);
        sumX += x; sumY += y; sumXX += x * x; sumXY += x * y;
    }

    const double n = (double) anchors.size();
    const double denominator = n * sumXX - sumX * sumX;

    if (anchors.size() >= 2 && denominator != 0.0)
        fit.nsPerSample = (n * sumXY - sumX * sumY) / denominator;

    const double interceptCentred = (sumY - fit.nsPerSample * sumX) / n;
    fit.interceptNs = (double) h0 + interceptCentred - fit.nsPerSample * (double) s0;

    double sumSquares = 0;
    for (auto& a : anchors)
    {
        const double x = (double) (a.songSample - s0);
        const double y = (double) (int64_t) (a.hostNs - h0);
        const double residual = y - (interceptCentred + fit.nsPerSample * x);
        fit.maxResidualNs = std::max (fit.maxResidualNs, std::abs (residual));
        sumSquares += residual * residual;
    }

    fit.rmsResidualNs = std::sqrt (sumSquares / n);
    return fit;
}

double songSampleHeardAtHostNs (const ClockFit& fit, double hostNs, double outputLatencyNs)
{
    return (hostNs - outputLatencyNs - fit.interceptNs) / fit.nsPerSample;
}

double songSampleHeardAtHostNsNaive (const ClockAnchor& first, double sampleRate, double hostNs, double outputLatencyNs)
{
    const double elapsedNs = hostNs - outputLatencyNs - (double) first.hostNs;
    return (double) first.songSample + elapsedNs * sampleRate / 1.0e9;
}

uint64_t hostNowNs()
{
    // Same timebase as CoreAudio's mHostTime and CMClockGetHostTimeClock().
    return clock_gettime_nsec_np (CLOCK_UPTIME_RAW);
}
