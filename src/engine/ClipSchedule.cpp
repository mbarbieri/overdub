#include "engine/ClipSchedule.h"

#include "song/ClipEdits.h"

#include <algorithm>
#include <cmath>

namespace overdub::engine
{
namespace
{
    // The parts of `segment` outside [coverStart, coverEnd), appended to `out`.
void appendUncovered (const ClipSegment& segment, SampleCount coverStart, SampleCount coverEnd, std::vector<ClipSegment>& out)
{
    if (segment.timelineEnd <= coverStart || segment.timelineStart >= coverEnd)
    {
        out.push_back (segment);
        return;
    }

    if (segment.timelineStart < coverStart)
    {
        auto before = segment;
        before.timelineEnd = coverStart;
        out.push_back (before);
    }

    if (segment.timelineEnd > coverEnd)
    {
        auto after = segment;
        after.timelineStart = coverEnd;
        after.sourceStart = segment.sourceStart + (coverEnd - segment.timelineStart);
        out.push_back (after);
    }
}

// Multiplies Song samples [from, to) of `out` (which starts at startSample) by the envelope.
void applyEnvelope (const ClipEnvelope& envelope, SampleCount from, SampleCount to, SampleCount startSample, juce::AudioBuffer<float>& out) noexcept
{
    const auto fadeInEnd = envelope.clipStart + envelope.fadeInLength;
    const auto fadeOutStart = envelope.clipStart + envelope.clipLength - envelope.fadeOutLength;
    const auto touchesFade = from < fadeInEnd || to > fadeOutStart;

    if (! touchesFade)
    {
        for (int channel = 0; channel < out.getNumChannels(); ++channel)
            juce::FloatVectorOperations::multiply (out.getWritePointer (channel, (int) (from - startSample)), (float) envelope.gain, (int) (to - from));
        return;
    }

    for (auto sample = from; sample < to; ++sample)
    {
        const auto amplitude = (float) envelopeAt (envelope, sample);
        for (int channel = 0; channel < out.getNumChannels(); ++channel)
            out.getWritePointer (channel)[sample - startSample] *= amplitude;
    }
}
} // namespace

double envelopeAt (const ClipEnvelope& envelope, SampleCount timelineSample) noexcept
{
    const auto k = timelineSample - envelope.clipStart;
    auto amplitude = envelope.gain;
    if (k < envelope.fadeInLength)
        amplitude *= (double) k / (double) envelope.fadeInLength;
    const auto fromEnd = envelope.clipLength - 1 - k;
    if (fromEnd < envelope.fadeOutLength)
        amplitude *= (double) fromEnd / (double) envelope.fadeOutLength;
    return amplitude;
}

std::vector<ClipSegment> flattenClips (std::span<const LayeredClip> clipsInLayerOrder)
{
    std::vector<ClipSegment> segments;

    for (const auto& clip : clipsInLayerOrder)
    {
        if (clip.length <= 0)
            continue;

        const ClipSegment top {
            .timelineStart = clip.timelineStart,
            .timelineEnd = clip.timelineStart + clip.length,
            .sourceStart = clip.sourceOffset,
            .source = clip.source.get(),
            .envelope = {
                .clipStart = clip.timelineStart,
                .clipLength = clip.length,
                .gain = song::linearGain (clip.gainDb),
                .fadeInLength = clip.fadeInLength,
                .fadeOutLength = clip.fadeOutLength,
            },
        };

        std::vector<ClipSegment> painted;
        painted.reserve (segments.size() + 2);
        for (const auto& segment : segments)
            appendUncovered (segment, top.timelineStart, top.timelineEnd, painted);
        painted.push_back (top);

        segments = std::move (painted);
    }

    std::ranges::sort (segments, {}, &ClipSegment::timelineStart);
    return segments;
}

void renderSegments (std::span<const ClipSegment> segments, SampleCount startSample, juce::AudioBuffer<float>& out) noexcept
{
    out.clear();

    const auto numSamples = (SampleCount) out.getNumSamples();
    const auto endSample = startSample + numSamples;

    // First segment that ends after startSample; segments are sorted and disjoint.
    auto segment = std::ranges::upper_bound (segments, startSample, {}, &ClipSegment::timelineEnd);

    for (; segment != segments.end() && segment->timelineStart < endSample; ++segment)
    {
        if (segment->source == nullptr)
            continue;

        const auto& source = *segment->source;
        const auto sourceLength = (SampleCount) source.getNumSamples();

        // Song range to copy, then narrowed to where the source has samples.
        auto from = std::max (startSample, segment->timelineStart);
        auto to = std::min (endSample, segment->timelineEnd);
        const auto sourceAtFrom = segment->sourceStart + (from - segment->timelineStart);
        if (sourceAtFrom < 0)
            from -= sourceAtFrom;
        to = std::min (to, from + (sourceLength - std::max<SampleCount> (sourceAtFrom, 0)));
        if (from >= to)
            continue;

        const auto sourceIndex = segment->sourceStart + (from - segment->timelineStart);
        for (int channel = 0; channel < out.getNumChannels(); ++channel)
        {
            const auto sourceChannel = std::min (channel, source.getNumChannels() - 1);
            juce::FloatVectorOperations::copy (out.getWritePointer (channel, (int) (from - startSample)),
                                               source.getReadPointer (sourceChannel, (int) sourceIndex),
                                               (int) (to - from));
        }
        applyEnvelope (segment->envelope, from, to, startSample, out);
    }
}
} // namespace overdub::engine
