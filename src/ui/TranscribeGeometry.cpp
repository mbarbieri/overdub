#include "ui/TranscribeGeometry.h"

#include "core/Decibels.h"
#include "engine/MixLaw.h"
#include "ui/RecordView.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <ranges>

namespace overdub::ui
{
namespace
{
SampleCount nextPowerOfTwo (SampleCount value)
{
    SampleCount power = 1;
    while (power < value)
        power *= 2;
    return power;
}

float decibelsToGain (double decibels)
{
    return (float) core::decibelsToGain (decibels);
}

// Which Clip (its index in layer order) is on top at each column's first sample; -1 for none.
std::vector<int> topClipPerColumn (const song::Track& track, SampleCount firstSample, SampleCount samplesPerPixel, int numColumns)
{
    std::vector<int> owner ((std::size_t) numColumns, -1);
    for (std::size_t index = 0; index < track.clips.size(); ++index)
    {
        const auto& clip = track.clips[index];
        // Columns whose first sample lies in [timelineStart, timelineStart + length).
        const auto relativeStart = clip.timelineStart - firstSample;
        const auto relativeEnd = relativeStart + clip.length;
        const auto from = std::max<SampleCount> (0, (relativeStart + samplesPerPixel - 1) / samplesPerPixel);
        const auto to = std::min<SampleCount> (numColumns, relativeEnd <= 0 ? 0 : (relativeEnd + samplesPerPixel - 1) / samplesPerPixel);
        for (auto column = from; column < to; ++column)
            owner[(std::size_t) column] = (int) index;
    }
    return owner;
}
} // namespace

SampleCount transcribeMinSamplesPerPixel (int sampleRate, int width)
{
    const auto exact = finestTranscribeSeconds * (double) sampleRate / (double) std::max (1, width);
    const auto levels = std::max (0.0, std::round (std::log2 (std::max (1.0, exact))));
    return (SampleCount) 1 << (int) levels;
}

SampleCount transcribeMaxSamplesPerPixel (SampleCount songLength, int width, SampleCount minimum)
{
    const auto fitting = (songLength + std::max (1, width) - 1) / std::max (1, width);
    return std::max (minimum, nextPowerOfTwo (std::max<SampleCount> (1, fitting)));
}

TranscribeZoomLimits transcribeZoomLimits (int sampleRate, SampleCount songLength, int width)
{
    const auto minimum = transcribeMinSamplesPerPixel (sampleRate, width);
    return { minimum, transcribeMaxSamplesPerPixel (songLength, width, minimum) };
}

TimelineView clampedTranscribeView (TimelineView view, SampleCount minimum, SampleCount maximum, SampleCount songLength, int width)
{
    view.samplesPerPixel = std::clamp (view.samplesPerPixel, minimum, std::max (minimum, maximum));
    const auto visible = view.samplesPerPixel * (SampleCount) std::max (1, width);
    view.firstSample = std::clamp<SampleCount> (view.firstSample, 0, std::max<SampleCount> (0, songLength - visible));
    return view;
}

int transcribeWheelPixels (float deltaX, float deltaY)
{
    return wheelScroll (deltaX, deltaY, true).timePixels;
}

float transcribeWheelZoomScale (float deltaY)
{
    return (float) std::exp2 ((double) deltaY * transcribeWheelZoomLevelsPerUnit);
}

SampleCount overviewViewStart (int x, int width, SampleCount songLength, SampleCount visibleSamples)
{
    return std::max<SampleCount> (0, overviewSampleAt (x, width, songLength) - visibleSamples / 2);
}

std::vector<song::PeakRange> heardMixColumns (const song::Song& song, const PeaksLookup& peaksOf, SampleCount firstSample, SampleCount samplesPerPixel, int numColumns)
{
    std::vector<song::PeakRange> mix ((std::size_t) std::max (0, numColumns));
    if (mix.empty() || samplesPerPixel <= 0 || ! peaksOf)
        return mix;

    const auto anySoloed = std::ranges::any_of (song.tracks, &song::Track::solo);
    const auto masterGain = decibelsToGain (song.master.volumeDb);
    for (const auto& track : song.tracks)
    {
        if (! engine::isAudible (track, anySoloed))
            continue;
        const auto trackGain = masterGain * decibelsToGain (track.volumeDb);
        const auto owner = topClipPerColumn (track, firstSample, samplesPerPixel, numColumns);
        for (std::size_t index = 0; index < track.clips.size(); ++index)
        {
            const auto first = std::ranges::find (owner, (int) index);
            if (first == owner.end())
                continue;
            const auto& clip = track.clips[index];
            const auto firstColumn = (int) (first - owner.begin());
            const auto columnStart = firstSample + (SampleCount) firstColumn * samplesPerPixel;
            const auto sourceStart = clip.sourceOffset + (columnStart - clip.timelineStart);
            const auto last = std::ranges::find (owner | std::views::reverse, (int) index);
            const auto endColumn = (int) (owner.rend() - last);
            const auto columns = waveformColumns (peaksOf (clip.source), sourceStart, samplesPerPixel, endColumn - firstColumn);
            const auto gain = trackGain * decibelsToGain (clip.gainDb);
            for (std::size_t i = 0; i < columns.size(); ++i)
            {
                const auto column = (std::size_t) firstColumn + i;
                if (owner[column] != (int) index)
                    continue;
                mix[column].minimum += columns[i].minimum * gain;
                mix[column].maximum += columns[i].maximum * gain;
            }
        }
    }
    for (auto& column : mix)
        column = { std::clamp (column.minimum, -1.0f, 1.0f), std::clamp (column.maximum, -1.0f, 1.0f) };
    return mix;
}
} // namespace overdub::ui
