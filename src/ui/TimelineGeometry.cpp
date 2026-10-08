#include "ui/TimelineGeometry.h"

#include "song/ClipEdits.h"

#include <algorithm>
#include <cmath>
#include <format>

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

ClipHandleBounds clipHandleBounds (const TimelineView& view, const song::Clip& clip, int clipTop)
{
    const auto columns = clipColumns (view, clip);
    const auto handleLeft = [&] (SampleCount fadeEnd) {
        const auto centred = xForSample (view, fadeEnd) - fadeHandleSize / 2;
        return std::max (columns.left, std::min (centred, columns.right - fadeHandleSize));
    };

    ClipHandleBounds bounds;
    bounds.fadeIn = { handleLeft (clip.timelineStart + clip.fadeInLength), clipTop, fadeHandleSize, fadeHandleSize };
    bounds.fadeOut = { handleLeft (clip.timelineStart + clip.length - clip.fadeOutLength), clipTop, fadeHandleSize, fadeHandleSize };
    if (columns.right - columns.left >= gainTagWidth + 2 * clipEdgeGrabWidth + 4)
        bounds.gainTag = { columns.right - clipEdgeGrabWidth - 2 - gainTagWidth, clipTop + fadeHandleSize + 1, gainTagWidth, gainTagHeight };
    return bounds;
}

std::optional<ClipPart> hitTestClipHandles (const TimelineView& view, const song::Clip& clip, int clipTop, int x, int y)
{
    const auto bounds = clipHandleBounds (view, clip, clipTop);
    if (bounds.fadeIn.contains (x, y))
        return ClipPart::fadeInHandle;
    if (bounds.fadeOut.contains (x, y))
        return ClipPart::fadeOutHandle;
    if (bounds.gainTag.contains (x, y))
        return ClipPart::gainTag;
    return std::nullopt;
}

core::Expected<song::Song> songForDrag (const song::Song& songAtDown, const ClipDrag& drag, const TimelineView& view, int x, int y)
{
    const auto* clip = song::findClip (songAtDown, drag.clipId);
    if (clip == nullptr)
        return std::unexpected (song::clipNotFound (drag.clipId));

    const auto delta = (SampleCount) (x - drag.downX) * view.samplesPerPixel;
    switch (drag.part)
    {
        case ClipPart::body:
            return song::moveClip (songAtDown, drag.clipId, clip->timelineStart + delta);
        case ClipPart::startEdge:
            return song::trimClipStart (songAtDown, drag.clipId, clip->timelineStart + delta);
        case ClipPart::endEdge:
            return song::trimClipEnd (songAtDown, drag.clipId, clip->timelineStart + clip->length + delta);
        case ClipPart::fadeInHandle:
            return song::setClipFadeIn (songAtDown, drag.clipId, clip->fadeInLength + delta);
        case ClipPart::fadeOutHandle:
            return song::setClipFadeOut (songAtDown, drag.clipId, clip->fadeOutLength - delta);
        case ClipPart::gainTag:
        {
            const auto gainDb = clip->gainDb - (double) (y - drag.downY) * gainDbPerPixel;
            return song::setClipGain (songAtDown, drag.clipId, std::round (gainDb * 10.0) / 10.0);
        }
    }
    return songAtDown;
}

std::string decibelsText (double gainDb)
{
    const auto rounded = std::round (gainDb * 10.0) / 10.0;
    if (std::abs (rounded) < 0.05)
        return "0.0 dB";
    return std::format ("{:+.1f} dB", rounded);
}

const char* labelForDrag (ClipPart part)
{
    switch (part)
    {
        case ClipPart::body:
            return song::moveClipLabel;
        case ClipPart::startEdge:
        case ClipPart::endEdge:
            return song::trimClipLabel;
        case ClipPart::fadeInHandle:
            return song::fadeInLabel;
        case ClipPart::fadeOutHandle:
            return song::fadeOutLabel;
        case ClipPart::gainTag:
            return song::clipGainLabel;
    }
    return song::moveClipLabel;
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
