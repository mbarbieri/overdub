#include "capture/ClapMeasurement.h"

#include <algorithm>
#include <cmath>

namespace overdub::capture
{
namespace
{
constexpr float onsetFractionOfPeak = 0.3f;
constexpr float quietestOnset = 0.01f;
constexpr double refractorySeconds = 0.25;
constexpr double clapWindowSeconds = 0.3;
// A frame-to-frame change this small is sensor noise, not hands.
constexpr double stillMotion = 1.0;

double median (std::vector<double> values)
{
    if (values.empty())
        return 0.0;
    const auto middle = values.begin() + (std::ptrdiff_t) (values.size() / 2);
    std::nth_element (values.begin(), middle, values.end());
    return *middle;
}

double medianFrameIntervalMs (const std::vector<MovieFrame>& frames)
{
    std::vector<double> intervals;
    for (std::size_t i = 1; i < frames.size(); ++i)
        intervals.push_back ((frames[i].seconds - frames[i - 1].seconds) * 1000.0);
    return median (std::move (intervals));
}
} // namespace

std::vector<SampleCount> findClaps (const std::vector<float>& samples, int sampleRate)
{
    float peak = 0.0f;
    for (const auto sample : samples)
        peak = std::max (peak, std::abs (sample));

    const auto threshold = std::max (onsetFractionOfPeak * peak, quietestOnset);
    const auto refractory = (SampleCount) (refractorySeconds * sampleRate);
    std::vector<SampleCount> claps;
    for (SampleCount i = 0; i < (SampleCount) samples.size(); ++i)
    {
        if (std::abs (samples[(std::size_t) i]) < threshold)
            continue;
        if (! claps.empty() && i - claps.back() < refractory)
            continue;
        claps.push_back (i);
    }
    return claps;
}

std::optional<double> visualClapSeconds (const std::vector<MovieFrame>& frames, double aroundSeconds, double windowSeconds)
{
    std::size_t first = frames.size(), last = 0;
    for (std::size_t i = 0; i < frames.size(); ++i)
    {
        if (std::abs (frames[i].seconds - aroundSeconds) > windowSeconds)
            continue;
        first = std::min (first, i);
        last = std::max (last, i);
    }
    if (first >= frames.size())
        return std::nullopt;

    std::vector<double> motions;
    auto peak = first;
    for (auto i = first; i <= last; ++i)
    {
        motions.push_back (frames[i].motion);
        if (frames[i].motion > frames[peak].motion)
            peak = i;
    }
    if (frames[peak].motion < std::max (stillMotion, 3.0 * median (motions)))
        return std::nullopt;

    // The burst's last frame: the hands met after the frame before it.
    auto end = peak;
    while (end + 1 <= last && frames[end + 1].motion >= 0.5 * frames[peak].motion)
        ++end;
    if (end == 0)
        return std::nullopt;
    return (frames[end - 1].seconds + frames[end].seconds) / 2.0;
}

ClapReport measureClaps (const std::vector<float>& takeSamples, const ClapPlacement& placement, const std::vector<MovieFrame>& frames)
{
    ClapReport report;
    report.frameIntervalMs = medianFrameIntervalMs (frames);
    const auto movieEnd = frames.empty() ? 0.0 : frames.back().seconds;

    double offsetSum = 0.0;
    int seen = 0;
    bool allWithinHalfFrame = true;
    for (const auto onset : findClaps (takeSamples, placement.sampleRate))
    {
        ClapResult clap;
        clap.songSample = placement.takeOrigin + onset;
        clap.audioMovieSeconds = (double) (clap.songSample - placement.videoStartSample) / placement.songSamplesPerMovieSecond;
        if (clap.audioMovieSeconds >= 0.0 && clap.audioMovieSeconds <= movieEnd)
            clap.visualMovieSeconds = visualClapSeconds (frames, clap.audioMovieSeconds, clapWindowSeconds);
        if (clap.visualMovieSeconds.has_value())
        {
            clap.offsetMs = (*clap.visualMovieSeconds - clap.audioMovieSeconds) * 1000.0;
            offsetSum += *clap.offsetMs;
            ++seen;
            // A hair of slack: a clap exactly half a frame off is within ±½ frame.
            allWithinHalfFrame = allWithinHalfFrame && std::abs (*clap.offsetMs) <= report.frameIntervalMs / 2.0 + 1.0e-6;
        }
        report.claps.push_back (clap);
    }
    if (seen > 0)
        report.meanOffsetMs = offsetSum / seen;
    report.pass = seen > 0 && allWithinHalfFrame;
    return report;
}

core::Fields clapReportFields (const ClapReport& report)
{
    auto claps = core::Fields::array();
    for (const auto& clap : report.claps)
    {
        claps.push_back ({
            { "song_sample", clap.songSample },
            { "audio_movie_seconds", clap.audioMovieSeconds },
            { "visual_movie_seconds", clap.visualMovieSeconds.has_value() ? core::Fields (*clap.visualMovieSeconds) : core::Fields (nullptr) },
            { "offset_ms", clap.offsetMs.has_value() ? core::Fields (*clap.offsetMs) : core::Fields (nullptr) },
        });
    }
    return {
        { "pass", report.pass },
        { "frame_interval_ms", report.frameIntervalMs },
        { "half_frame_ms", report.frameIntervalMs / 2.0 },
        { "mean_offset_ms", report.meanOffsetMs.has_value() ? core::Fields (*report.meanOffsetMs) : core::Fields (nullptr) },
        { "claps", claps },
    };
}
} // namespace overdub::capture
