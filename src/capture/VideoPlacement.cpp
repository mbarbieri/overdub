#include "capture/VideoPlacement.h"

#include <algorithm>
#include <cmath>

namespace overdub::capture
{
namespace
{
double nominalNsPerSample (int sampleRate)
{
    return 1.0e9 / (double) sampleRate;
}

// Host time relative to the fit's origin, in ns, without going through a large double.
double relativeHostNs (const AnchorFit& fit, std::uint64_t hostNs)
{
    return (double) (std::int64_t) (hostNs - fit.originHostNs);
}
} // namespace

double AnchorFit::driftPpm (int sampleRate) const
{
    return (nominalNsPerSample (sampleRate) / nsPerSample - 1.0) * 1.0e6;
}

std::optional<AnchorFit> fitAnchors (const std::vector<engine::Anchor>& anchors, int sampleRate)
{
    if (anchors.empty())
        return std::nullopt;

    AnchorFit fit;
    fit.originSample = anchors.front().songSample;
    fit.originHostNs = anchors.front().hostTimeNs;
    fit.nsPerSample = nominalNsPerSample (sampleRate);
    fit.anchorCount = (int) anchors.size();

    const auto x = [&] (const engine::Anchor& anchor) { return (double) (anchor.songSample - fit.originSample); };
    const auto y = [&] (const engine::Anchor& anchor) { return relativeHostNs (fit, anchor.hostTimeNs); };

    // Two passes (means, then centred sums) keep the sums small over long Passes.
    double meanX = 0.0, meanY = 0.0;
    for (const auto& anchor : anchors)
    {
        meanX += x (anchor);
        meanY += y (anchor);
    }
    meanX /= (double) anchors.size();
    meanY /= (double) anchors.size();

    double sxx = 0.0, sxy = 0.0;
    for (const auto& anchor : anchors)
    {
        const auto dx = x (anchor) - meanX;
        sxx += dx * dx;
        sxy += dx * (y (anchor) - meanY);
    }
    if (sxx > 0.0)
        fit.nsPerSample = sxy / sxx;
    fit.hostOffsetNs = meanY - fit.nsPerSample * meanX;

    for (const auto& anchor : anchors)
        fit.maxResidualNs = std::max (fit.maxResidualNs, std::abs (y (anchor) - (fit.hostOffsetNs + fit.nsPerSample * x (anchor))));
    return fit;
}

double songSampleHeardAt (const AnchorFit& fit, std::uint64_t hostNs, int outputLatencySamples)
{
    const auto atOutput = (relativeHostNs (fit, hostNs) - fit.hostOffsetNs) / fit.nsPerSample;
    return (double) fit.originSample + atOutput - (double) outputLatencySamples;
}

VideoPlacement placeVideoTake (const AnchorFit& fit,
                               std::uint64_t firstFrameHostNs,
                               int outputLatencySamples,
                               SampleCount calibrationOffsetSamples,
                               double captureClockRate,
                               int sampleRate)
{
    VideoPlacement placement;
    const auto heard = songSampleHeardAt (fit, firstFrameHostNs, outputLatencySamples) - (double) calibrationOffsetSamples;
    placement.startSample = (SampleCount) std::llround (heard);
    // One movie second is 1 / captureClockRate host seconds.
    placement.songSamplesPerMovieSecond = 1.0e9 / fit.nsPerSample / captureClockRate;
    placement.driftPpm = (placement.songSamplesPerMovieSecond / (double) sampleRate - 1.0) * 1.0e6;
    return placement;
}

double songSampleAtMovieTime (SampleCount startSample, double songSamplesPerMovieSecond, double movieSeconds)
{
    return (double) startSample + movieSeconds * songSamplesPerMovieSecond;
}
} // namespace overdub::capture
