#include "video/FrameSelection.h"

#include "song/TakeEdits.h"

#include <algorithm>
#include <cmath>

namespace overdub::video
{
namespace
{
// Frame times and movie times come from different arithmetic (PTS seconds vs Song samples over a
// rate): a frame this close after the movie time counts as at it.
constexpr double sameTimeSeconds = 1.0e-9;

// The Pass's place in the Song's Pass order (later = newer); -1 when the Song lacks it.
int passOrder (const song::Song& song, const song::Id& passId)
{
    const auto found = std::ranges::find (song.passes, passId, &song::Pass::id);
    return found != song.passes.end() ? (int) std::distance (song.passes.begin(), found) : -1;
}
} // namespace

double heardSongSample (const PlaybackClock& clock, std::uint64_t nowHostNs, std::uint64_t refreshNs)
{
    if (! clock.playing || ! clock.anchor.has_value())
        return (double) clock.position;
    // Signed: `now` may be read just before the audio thread published a newer anchor.
    const auto sinceAnchorNs = std::min ((double) (std::int64_t) (nowHostNs - clock.anchor->hostTimeNs), (double) maxExtrapolationNs);
    const auto elapsedNs = sinceAnchorNs + (double) refreshNs;
    return (double) clock.anchor->songSample + elapsedNs * (double) clock.sampleRate / 1.0e9 - (double) clock.outputLatencySamples;
}

SampleCount videoTakeEnd (const song::VideoTake& videoTake, const MovieTiming& timing)
{
    const auto length = (SampleCount) std::llround (timing.durationSeconds * videoTake.songSamplesPerMovieSecond);
    return videoTake.startSample + std::max<SampleCount> (0, length);
}

std::vector<VideoTakeSpan> videoTakeSpans (const song::Song& song, const TimingLookup& timingOf)
{
    std::vector<VideoTakeSpan> spans;
    spans.reserve (song.videoTakes.size());
    for (const auto& videoTake : song.videoTakes)
    {
        const auto* timing = timingOf ? timingOf (videoTake) : nullptr;
        const auto readable = timing != nullptr && ! timing->frameSeconds.empty();
        spans.push_back (VideoTakeSpan {
            .videoTakeId = videoTake.id,
            .title = "Pass " + std::to_string (song::passNumber (song, videoTake.passId)),
            .start = videoTake.startSample,
            .end = readable ? videoTakeEnd (videoTake, *timing) : videoTake.startSample,
            .readable = readable,
        });
    }
    return spans;
}

double movieSecondsAt (const song::VideoTake& videoTake, double songSample)
{
    if (videoTake.songSamplesPerMovieSecond <= 0.0)
        return 0.0;
    return (songSample - (double) videoTake.startSample) / videoTake.songSamplesPerMovieSecond;
}

std::optional<std::size_t> frameIndexAt (const std::vector<double>& frameSeconds, double movieSeconds)
{
    const auto after = std::upper_bound (frameSeconds.begin(), frameSeconds.end(), movieSeconds + sameTimeSeconds);
    if (after == frameSeconds.begin())
        return std::nullopt;
    return (std::size_t) std::distance (frameSeconds.begin(), after) - 1;
}

FrameChoice chooseFrame (const song::Song& song, const TimingLookup& timingOf, double heardSample)
{
    const song::VideoTake* chosen = nullptr;
    const MovieTiming* chosenTiming = nullptr;
    int chosenOrder = -2;
    const auto spans = videoTakeSpans (song, timingOf);
    for (std::size_t i = 0; i < spans.size(); ++i)
    {
        const auto& span = spans[i];
        if (! span.readable || heardSample < (double) span.start || heardSample >= (double) span.end)
            continue;
        const auto& videoTake = song.videoTakes[i];
        const auto order = passOrder (song, videoTake.passId);
        if (order < chosenOrder)
            continue;
        chosen = &videoTake;
        chosenTiming = timingOf (videoTake);
        chosenOrder = order;
    }
    if (chosen == nullptr)
        return {};

    const auto movieSeconds = movieSecondsAt (*chosen, heardSample);
    return FrameChoice {
        .videoTakeId = chosen->id,
        .frameIndex = frameIndexAt (chosenTiming->frameSeconds, movieSeconds),
        .movieSeconds = movieSeconds,
    };
}
} // namespace overdub::video
