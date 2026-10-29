#include "ui/TimelineGeometry.h"

#include "song/ClipEdits.h"
#include "ui/SongEdits.h"

#include <algorithm>
#include <array>
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

WheelScroll wheelScroll (float deltaX, float deltaY, bool shift)
{
    constexpr float pixelsPerUnit = 400.0f;
    const auto pixels = [] (float delta) { return (int) std::lround (-delta * pixelsPerUnit); };
    if (shift)
        return { .timePixels = pixels (deltaX != 0.0f ? deltaX : deltaY), .verticalPixels = 0 };
    return { .timePixels = pixels (deltaX), .verticalPixels = pixels (deltaY) };
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

PinchZoom pinchZoomed (TimelineView view, double pendingLevels, float scaleFactor, int anchorX, SampleCount minimum, SampleCount maximum)
{
    auto levels = pendingLevels + std::log2 ((double) scaleFactor);
    auto samplesPerPixel = view.samplesPerPixel;
    while (levels >= 0.5 && samplesPerPixel > minimum)
    {
        samplesPerPixel = std::max (minimum, samplesPerPixel / 2);
        levels -= 1.0;
    }
    while (levels <= -0.5 && samplesPerPixel < maximum)
    {
        samplesPerPixel = std::min (maximum, samplesPerPixel * 2);
        levels += 1.0;
    }
    if (std::abs (levels) >= 0.5)
        levels = 0.0;

    const auto anchor = std::max (anchorX, view.laneX);
    const auto anchorSample = sampleForX (view, anchor);
    view.samplesPerPixel = samplesPerPixel;
    view.firstSample = std::max<SampleCount> (0, anchorSample - (SampleCount) (anchor - view.laneX) * samplesPerPixel);
    return { view, levels };
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

core::Expected<intent::Edit> intentForDrag (const song::Song& songAtDown, const ClipDrag& drag, const TimelineView& view, int x, int y)
{
    const auto* clip = song::findClip (songAtDown, drag.clipId);
    if (clip == nullptr)
        return std::unexpected (song::clipNotFound (drag.clipId));

    const auto delta = (SampleCount) (x - drag.downX) * view.samplesPerPixel;
    switch (drag.part)
    {
        case ClipPart::body:
            return intent::MoveClip { drag.clipId, clip->timelineStart + delta };
        case ClipPart::startEdge:
            return intent::TrimClipStart { drag.clipId, clip->timelineStart + delta };
        case ClipPart::endEdge:
            return intent::TrimClipEnd { drag.clipId, clip->timelineStart + clip->length + delta };
        case ClipPart::fadeInHandle:
            return intent::SetClipFadeIn { drag.clipId, clip->fadeInLength + delta };
        case ClipPart::fadeOutHandle:
            return intent::SetClipFadeOut { drag.clipId, clip->fadeOutLength - delta };
        case ClipPart::gainTag:
        {
            const auto gainDb = clip->gainDb - (double) (y - drag.downY) * gainDbPerPixel;
            return intent::SetClipGain { drag.clipId, std::round (gainDb * 10.0) / 10.0 };
        }
    }
}

core::Expected<song::Song> songForDrag (const song::Song& songAtDown, const ClipDrag& drag, const TimelineView& view, int x, int y)
{
    const auto edit = intentForDrag (songAtDown, drag, view, x, y);
    if (! edit.has_value())
        return std::unexpected (edit.error());
    auto edited = editedSong (songAtDown, *edit);
    if (! edited.has_value())
        return std::unexpected (edited.error());
    return std::move (edited->song);
}

double rulerStepSeconds (SampleCount samplesPerPixel, int sampleRate)
{
    constexpr std::array steps { 0.1, 0.25, 0.5, 1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 60.0, 120.0, 300.0 };
    for (const auto step : steps)
        if (step * sampleRate / (double) samplesPerPixel >= 70.0)
            return step;
    return steps.back();
}

std::string rulerLabel (double seconds)
{
    return std::format ("{:g} s", seconds);
}

std::string decibelsText (double gainDb)
{
    const auto rounded = std::round (gainDb * 10.0) / 10.0;
    if (std::abs (rounded) < 0.05)
        return "0.0 dB";
    return std::format ("{:+.1f} dB", rounded);
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
