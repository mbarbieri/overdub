#include "song/ClipEdits.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace overdub::song
{
namespace
{
struct ClipPosition
{
    std::size_t track = 0;
    std::size_t clip = 0;
};

std::optional<ClipPosition> locateClip (const Song& song, const Id& clipId)
{
    for (std::size_t t = 0; t < song.tracks.size(); ++t)
        for (std::size_t c = 0; c < song.tracks[t].clips.size(); ++c)
            if (song.tracks[t].clips[c].id == clipId)
                return ClipPosition { t, c };
    return std::nullopt;
}

void clampFades (Clip& clip)
{
    clip.fadeInLength = std::clamp<SampleCount> (clip.fadeInLength, 0, clip.length);
    clip.fadeOutLength = std::clamp<SampleCount> (clip.fadeOutLength, 0, clip.length - clip.fadeInLength);
}
} // namespace

core::Error clipNotFound (const Id& clipId)
{
    return core::Error { "song.clip_not_found", "no Clip with this id in the Song", { { "clip_id", clipId } } };
}

const Clip* findClip (const Song& song, const Id& clipId)
{
    const auto position = locateClip (song, clipId);
    return position.has_value() ? &song.tracks[position->track].clips[position->clip] : nullptr;
}

const Track* trackOfClip (const Song& song, const Id& clipId)
{
    const auto position = locateClip (song, clipId);
    return position.has_value() ? &song.tracks[position->track] : nullptr;
}

SampleCount sourceLength (const Song& song, const ClipSource& source)
{
    if (source.kind == ClipSourceKind::take)
    {
        const auto take = std::ranges::find (song.takes, source.id, &Take::id);
        return take != song.takes.end() ? take->lengthSamples : 0;
    }
    const auto backing = std::ranges::find (song.backingTracks, source.id, &BackingTrack::id);
    return backing != song.backingTracks.end() ? backing->lengthSamples : 0;
}

core::Expected<Song> moveClip (Song song, const Id& clipId, SampleCount newTimelineStart)
{
    const auto position = locateClip (song, clipId);
    if (! position.has_value())
        return std::unexpected (clipNotFound (clipId));
    song.tracks[position->track].clips[position->clip].timelineStart = std::max<SampleCount> (0, newTimelineStart);
    return song;
}

core::Expected<Song> trimClipStart (Song song, const Id& clipId, SampleCount newTimelineStart)
{
    const auto position = locateClip (song, clipId);
    if (! position.has_value())
        return std::unexpected (clipNotFound (clipId));
    auto& clip = song.tracks[position->track].clips[position->clip];

    // delta > 0 shortens the Clip from the left; delta < 0 reveals earlier source samples.
    const auto lowest = -std::min (clip.sourceOffset, clip.timelineStart);
    const auto highest = clip.length - 1;
    const auto delta = std::clamp (newTimelineStart - clip.timelineStart, lowest, highest);
    clip.timelineStart += delta;
    clip.sourceOffset += delta;
    clip.length -= delta;
    clampFades (clip);
    return song;
}

core::Expected<Song> trimClipEnd (Song song, const Id& clipId, SampleCount newTimelineEnd)
{
    const auto position = locateClip (song, clipId);
    if (! position.has_value())
        return std::unexpected (clipNotFound (clipId));
    auto& clip = song.tracks[position->track].clips[position->clip];

    // A source the Song lacks has no known end: the Clip may shrink but not grow.
    const auto knownLength = sourceLength (song, clip.source);
    const auto longest = knownLength > 0 ? std::max<SampleCount> (1, knownLength - clip.sourceOffset) : clip.length;
    clip.length = std::clamp<SampleCount> (newTimelineEnd - clip.timelineStart, 1, longest);
    clampFades (clip);
    return song;
}

core::Expected<Song> splitClip (Song song, const Id& clipId, SampleCount at, const Id& rightClipId)
{
    const auto position = locateClip (song, clipId);
    if (! position.has_value())
        return std::unexpected (clipNotFound (clipId));
    auto& clips = song.tracks[position->track].clips;
    auto left = clips[position->clip];

    if (at <= left.timelineStart || at >= left.timelineStart + left.length)
        return std::unexpected (core::Error { "song.split_outside_clip", "the split point is not inside the Clip", { { "clip_id", clipId }, { "at", at } } });

    const auto cut = at - left.timelineStart;
    auto right = left;
    right.id = rightClipId;
    right.timelineStart = at;
    right.sourceOffset = left.sourceOffset + cut;
    right.length = left.length - cut;
    right.fadeInLength = 0;
    left.length = cut;
    left.fadeOutLength = 0;
    clampFades (left);
    clampFades (right);

    clips[position->clip] = left;
    clips.insert (clips.begin() + (std::ptrdiff_t) position->clip + 1, right);
    return song;
}

core::Expected<Song> setClipFadeIn (Song song, const Id& clipId, SampleCount length)
{
    const auto position = locateClip (song, clipId);
    if (! position.has_value())
        return std::unexpected (clipNotFound (clipId));
    auto& clip = song.tracks[position->track].clips[position->clip];
    clip.fadeInLength = std::clamp<SampleCount> (length, 0, std::max<SampleCount> (0, clip.length - clip.fadeOutLength));
    return song;
}

core::Expected<Song> setClipFadeOut (Song song, const Id& clipId, SampleCount length)
{
    const auto position = locateClip (song, clipId);
    if (! position.has_value())
        return std::unexpected (clipNotFound (clipId));
    auto& clip = song.tracks[position->track].clips[position->clip];
    clip.fadeOutLength = std::clamp<SampleCount> (length, 0, std::max<SampleCount> (0, clip.length - clip.fadeInLength));
    return song;
}

core::Expected<Song> setClipGain (Song song, const Id& clipId, double gainDb)
{
    const auto position = locateClip (song, clipId);
    if (! position.has_value())
        return std::unexpected (clipNotFound (clipId));
    song.tracks[position->track].clips[position->clip].gainDb = std::clamp (gainDb, minClipGainDb, maxClipGainDb);
    return song;
}

core::Expected<Song> deleteClip (Song song, const Id& clipId)
{
    const auto position = locateClip (song, clipId);
    if (! position.has_value())
        return std::unexpected (clipNotFound (clipId));
    auto& clips = song.tracks[position->track].clips;
    clips.erase (clips.begin() + (std::ptrdiff_t) position->clip);
    return song;
}

std::vector<SampleSpan> coveredSpans (const Track& track)
{
    std::vector<SampleSpan> spans;
    const auto& clips = track.clips;
    for (std::size_t lower = 0; lower < clips.size(); ++lower)
    {
        for (std::size_t higher = lower + 1; higher < clips.size(); ++higher)
        {
            const auto start = std::max (clips[lower].timelineStart, clips[higher].timelineStart);
            const auto end = std::min (clips[lower].timelineStart + clips[lower].length, clips[higher].timelineStart + clips[higher].length);
            if (end > start)
                spans.push_back ({ start, end });
        }
    }

    std::ranges::sort (spans, {}, &SampleSpan::start);
    std::vector<SampleSpan> merged;
    for (const auto& span : spans)
    {
        if (! merged.empty() && span.start <= merged.back().end)
            merged.back().end = std::max (merged.back().end, span.end);
        else
            merged.push_back (span);
    }
    return merged;
}
} // namespace overdub::song
