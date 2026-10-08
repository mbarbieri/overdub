#include "song/TakeEdits.h"

#include <algorithm>
#include <format>

namespace overdub::song
{
namespace
{
core::Error takeNotFound (const Id& takeId)
{
    return core::Error { "song.take_not_found", "no Take with this id in the Song", { { "take_id", takeId } } };
}

SampleCount passStartOf (const Song& song, const Take& take)
{
    const auto pass = std::ranges::find (song.passes, take.passId, &Pass::id);
    return pass != song.passes.end() ? pass->startSample : take.origin;
}

Track* mutableTrack (Song& song, const Id& trackId)
{
    const auto track = std::ranges::find (song.tracks, trackId, &Track::id);
    return track != song.tracks.end() ? &*track : nullptr;
}

SampleCount overlapLength (const std::vector<SampleSpan>& spans, SampleSpan within)
{
    SampleCount total = 0;
    for (const auto& span : spans)
        total += std::max<SampleCount> (0, std::min (span.end, within.end) - std::max (span.start, within.start));
    return total;
}

// How much of the Take's recorded span is heard; both 0 when its Track is gone.
struct HeardAmounts
{
    SampleCount recorded = 0;
    SampleCount heard = 0;
};

HeardAmounts heardAmounts (const Song& song, const Id& takeId)
{
    const auto* take = findTake (song, takeId);
    if (take == nullptr || findTrack (song, take->trackId) == nullptr)
        return {};
    const auto span = recordedSpan (song, takeId);
    if (! span.has_value())
        return {};
    return { span->end - span->start, overlapLength (heardSpans (song, takeId), *span) };
}

HeardStatus statusOf (SampleCount heard, SampleCount recorded)
{
    if (heard == 0)
        return HeardStatus::notHeard;
    return heard >= recorded ? HeardStatus::heard : HeardStatus::partlyHeard;
}

std::string trackName (const Song& song, const Id& trackId)
{
    const auto* track = findTrack (song, trackId);
    return track != nullptr ? track->name : "a deleted Track";
}
} // namespace

std::string_view toString (HeardStatus status)
{
    switch (status)
    {
        case HeardStatus::heard:
            return "heard";
        case HeardStatus::partlyHeard:
            return "partly heard";
        case HeardStatus::notHeard:
            return "not heard";
    }
    return "not heard";
}

int passNumber (const Song& song, const Id& passId)
{
    const auto pass = std::ranges::find (song.passes, passId, &Pass::id);
    return pass != song.passes.end() ? (int) (pass - song.passes.begin()) + 1 : 0;
}

const Take* findTake (const Song& song, const Id& takeId)
{
    const auto take = std::ranges::find (song.takes, takeId, &Take::id);
    return take != song.takes.end() ? &*take : nullptr;
}

const Track* findTrack (const Song& song, const Id& trackId)
{
    const auto track = std::ranges::find (song.tracks, trackId, &Track::id);
    return track != song.tracks.end() ? &*track : nullptr;
}

std::optional<Clip> clipAsRecorded (const Take& take, SampleCount passStart, const Id& clipId)
{
    const auto timelineStart = std::max (passStart, take.origin);
    const auto sourceOffset = timelineStart - take.origin;
    const auto length = take.lengthSamples - sourceOffset;
    if (length <= 0)
        return std::nullopt;

    Clip clip;
    clip.id = clipId;
    clip.source = ClipSource { ClipSourceKind::take, take.id };
    clip.timelineStart = timelineStart;
    clip.sourceOffset = sourceOffset;
    clip.length = length;
    return clip;
}

std::optional<SampleSpan> recordedSpan (const Song& song, const Id& takeId)
{
    const auto* take = findTake (song, takeId);
    if (take == nullptr)
        return std::nullopt;
    const auto clip = clipAsRecorded (*take, passStartOf (song, *take), {});
    if (! clip.has_value())
        return std::nullopt;
    return SampleSpan { clip->timelineStart, clip->timelineStart + clip->length };
}

const Clip* topClipAt (const Track& track, SampleCount at)
{
    for (auto clip = track.clips.rbegin(); clip != track.clips.rend(); ++clip)
        if (at >= clip->timelineStart && at < clip->timelineStart + clip->length)
            return &*clip;
    return nullptr;
}

std::vector<SampleSpan> heardSpans (const Song& song, const Id& takeId)
{
    const auto* take = findTake (song, takeId);
    const auto* track = take != nullptr ? findTrack (song, take->trackId) : nullptr;
    if (track == nullptr)
        return {};

    // Between two consecutive Clip edges the top Clip does not change.
    std::vector<SampleCount> edges;
    for (const auto& clip : track->clips)
    {
        edges.push_back (clip.timelineStart);
        edges.push_back (clip.timelineStart + clip.length);
    }
    std::ranges::sort (edges);
    edges.erase (std::unique (edges.begin(), edges.end()), edges.end());

    std::vector<SampleSpan> spans;
    for (std::size_t i = 0; i + 1 < edges.size(); ++i)
    {
        const auto* top = topClipAt (*track, edges[i]);
        if (top == nullptr || top->source.kind != ClipSourceKind::take || top->source.id != takeId)
            continue;
        if (! spans.empty() && spans.back().end == edges[i])
            spans.back().end = edges[i + 1];
        else
            spans.push_back ({ edges[i], edges[i + 1] });
    }
    return spans;
}

HeardStatus takeHeardStatus (const Song& song, const Id& takeId)
{
    const auto amounts = heardAmounts (song, takeId);
    return statusOf (amounts.heard, amounts.recorded);
}

HeardStatus passHeardStatus (const Song& song, const Id& passId)
{
    SampleCount recorded = 0;
    SampleCount heard = 0;
    for (const auto& take : song.takes)
    {
        if (take.passId != passId)
            continue;
        const auto amounts = heardAmounts (song, take.id);
        recorded += amounts.recorded;
        heard += amounts.heard;
    }
    return statusOf (heard, recorded);
}

std::vector<const Take*> laneTakes (const Song& song, const Id& trackId)
{
    std::vector<const Take*> takes;
    for (const auto& take : song.takes)
        if (take.trackId == trackId)
            takes.push_back (&take);

    // Newest Pass first; a Take whose Pass the Song lacks sorts as Pass 0, last.
    std::ranges::stable_sort (takes, [&song] (const Take* a, const Take* b) { return passNumber (song, a->passId) > passNumber (song, b->passId); });
    return takes;
}

core::Expected<Song> swapClipTake (Song song, const Id& clipId, const Id& takeId)
{
    const auto* found = findClip (song, clipId);
    if (found == nullptr)
        return std::unexpected (clipNotFound (clipId));
    const auto* take = findTake (song, takeId);
    if (take == nullptr)
        return std::unexpected (takeNotFound (takeId));
    const auto span = recordedSpan (song, takeId).value_or (SampleSpan {});
    const auto start = std::max (found->timelineStart, span.start);
    const auto end = std::min (found->timelineStart + found->length, span.end);
    if (span.end <= span.start || end <= start)
        return std::unexpected (core::Error { "song.take_not_under_clip", "the Take has no audio under the Clip", { { "clip_id", clipId }, { "take_id", takeId } } });

    const auto origin = take->origin;
    auto* track = mutableTrack (song, trackOfClip (song, clipId)->id);
    auto& clip = *std::ranges::find (track->clips, clipId, &Clip::id);
    clip.source = ClipSource { ClipSourceKind::take, takeId };
    clip.timelineStart = start;
    clip.sourceOffset = start - origin;
    clip.length = end - start;
    clip.fadeInLength = std::clamp<SampleCount> (clip.fadeInLength, 0, clip.length);
    clip.fadeOutLength = std::clamp<SampleCount> (clip.fadeOutLength, 0, clip.length - clip.fadeInLength);
    return song;
}

core::Expected<Song> bringBackTake (Song song, const Id& takeId, const Id& newClipId)
{
    const auto* take = findTake (song, takeId);
    if (take == nullptr)
        return std::unexpected (takeNotFound (takeId));
    auto* track = mutableTrack (song, take->trackId);
    if (track == nullptr)
        return std::unexpected (core::Error { "song.take_track_missing", "the Track the Take was recorded on is gone", { { "take_id", takeId }, { "track_id", take->trackId } } });
    auto clip = clipAsRecorded (*take, passStartOf (song, *take), newClipId);
    if (! clip.has_value())
        return std::unexpected (core::Error { "song.take_has_no_audio", "the Take recorded nothing from its Pass start", { { "take_id", takeId } } });
    track->clips.push_back (std::move (*clip));
    return song;
}

core::Expected<Song> bringBackPass (Song song, const Id& passId)
{
    if (passNumber (song, passId) == 0)
        return std::unexpected (core::Error { "song.pass_not_found", "no Pass with this id in the Song", { { "pass_id", passId } } });

    std::size_t added = 0;
    for (const auto& take : song.takes)
    {
        if (take.passId != passId)
            continue;
        auto* track = mutableTrack (song, take.trackId);
        if (track == nullptr)
            continue; // its Track was deleted: skipped
        if (auto clip = clipAsRecorded (take, passStartOf (song, take), makeId()))
        {
            track->clips.push_back (std::move (*clip));
            ++added;
        }
    }
    if (added == 0)
        return std::unexpected (core::Error { "song.pass_has_no_takes", "the Pass has no Take on a Track that still exists", { { "pass_id", passId } } });
    return song;
}

std::string useTakeLabel (const Song& song, const Take& take)
{
    return std::format ("Use Pass {} on {}", passNumber (song, take.passId), trackName (song, take.trackId));
}

std::string bringBackTakeLabel (const Song& song, const Take& take)
{
    return std::format ("Bring back Pass {} on {}", passNumber (song, take.passId), trackName (song, take.trackId));
}

std::string bringBackPassLabel (const Song& song, const Id& passId)
{
    return std::format ("Bring back Pass {}", passNumber (song, passId));
}
} // namespace overdub::song
