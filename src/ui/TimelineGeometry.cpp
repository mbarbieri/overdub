#include "ui/TimelineGeometry.h"

#include "song/ClipEdits.h"

#include <algorithm>

namespace overdub::ui
{
namespace
{
// Division rounding towards negative infinity.
SampleCount floorDivide (SampleCount value, SampleCount divisor)
{
    const auto quotient = value / divisor;
    return (value % divisor != 0 && (value < 0) != (divisor < 0)) ? quotient - 1 : quotient;
}
} // namespace

int xForSample (const TimelineView& view, SampleCount sample)
{
    return view.laneX + (int) floorDivide (sample - view.firstSample, view.samplesPerPixel);
}

SampleCount sampleForX (const TimelineView& view, int x)
{
    return view.firstSample + (SampleCount) (x - view.laneX) * view.samplesPerPixel;
}

TimelineView zoomedIn (TimelineView view)
{
    view.samplesPerPixel = std::max (minSamplesPerPixel, view.samplesPerPixel / 2);
    return view;
}

TimelineView zoomedOut (TimelineView view)
{
    view.samplesPerPixel = std::min (maxSamplesPerPixel, view.samplesPerPixel * 2);
    return view;
}

ClipColumns clipColumns (const TimelineView& view, const song::Clip& clip)
{
    const auto left = xForSample (view, clip.timelineStart);
    const auto right = std::max (left + 2, xForSample (view, clip.timelineStart + clip.length));
    return { left, right };
}

std::optional<ClipHit> hitTestClips (const std::vector<song::Clip>& clipsInLayerOrder, const TimelineView& view, int x)
{
    for (auto clip = clipsInLayerOrder.rbegin(); clip != clipsInLayerOrder.rend(); ++clip)
    {
        const auto columns = clipColumns (view, *clip);
        if (x < columns.left || x >= columns.right)
            continue;

        const auto edge = std::min (clipEdgeGrabWidth, (columns.right - columns.left) / 3);
        if (x < columns.left + edge)
            return ClipHit { clip->id, ClipPart::startEdge };
        if (x >= columns.right - edge)
            return ClipHit { clip->id, ClipPart::endEdge };
        return ClipHit { clip->id, ClipPart::body };
    }
    return std::nullopt;
}

core::Expected<song::Song> songForDrag (const song::Song& songAtDown, const ClipDrag& drag, const TimelineView& view, int x)
{
    const auto* clip = song::findClip (songAtDown, drag.clipId);
    if (clip == nullptr)
        return std::unexpected (core::Error { "song.clip_not_found", "the dragged Clip is not in the Song", { { "clip_id", drag.clipId } } });

    const auto delta = (SampleCount) (x - drag.downX) * view.samplesPerPixel;
    switch (drag.part)
    {
        case ClipPart::body:
            return song::moveClip (songAtDown, drag.clipId, clip->timelineStart + delta);
        case ClipPart::startEdge:
            return song::trimClipStart (songAtDown, drag.clipId, clip->timelineStart + delta);
        case ClipPart::endEdge:
            return song::trimClipEnd (songAtDown, drag.clipId, clip->timelineStart + clip->length + delta);
    }
    return songAtDown;
}

const char* labelForDrag (ClipPart part)
{
    return part == ClipPart::body ? song::moveClipLabel : song::trimClipLabel;
}

std::vector<song::PeakRange> waveformColumns (const song::Peaks& peaks, SampleCount sourceStart, SampleCount samplesPerPixel, int numColumns)
{
    std::vector<song::PeakRange> columns ((std::size_t) std::max (0, numColumns));
    if (peaks.channels.empty() || samplesPerPixel <= 0)
        return columns;

    const auto numBlocks = (SampleCount) peaks.channels.front().size();
    for (std::size_t i = 0; i < columns.size(); ++i)
    {
        const auto from = sourceStart + (SampleCount) i * samplesPerPixel;
        const auto to = from + samplesPerPixel;
        if (to <= 0)
            continue;
        const auto firstBlock = std::max<SampleCount> (0, from) / song::samplesPerPeak;
        const auto endBlock = std::min (numBlocks, (to - 1) / song::samplesPerPeak + 1);
        if (firstBlock >= endBlock)
            continue;

        song::PeakRange range { 1.0e9f, -1.0e9f };
        for (const auto& channel : peaks.channels)
        {
            for (auto block = firstBlock; block < endBlock; ++block)
            {
                range.minimum = std::min (range.minimum, channel[(std::size_t) block].minimum);
                range.maximum = std::max (range.maximum, channel[(std::size_t) block].maximum);
            }
        }
        columns[i] = range;
    }
    return columns;
}
} // namespace overdub::ui
