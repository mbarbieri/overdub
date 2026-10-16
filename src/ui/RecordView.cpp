#include "ui/RecordView.h"

#include "core/Decibels.h"
#include "engine/ClipSchedule.h"
#include "engine/MixLaw.h"
#include "ui/TransportText.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace overdub::ui
{
namespace
{
SampleCount pointsCovering (SampleCount samples)
{
    return (samples + song::samplesPerPeak - 1) / song::samplesPerPeak;
}

// The loudest |min| or |max| over every channel at peak index `index`; 0 outside the peaks.
float peakLevelAt (const song::Peaks& peaks, SampleCount index)
{
    float level = 0.0f;
    for (const auto& channel : peaks.channels)
    {
        if (index < 0 || index >= (SampleCount) channel.size())
            continue;
        const auto& range = channel[(std::size_t) index];
        level = std::max ({ level, std::abs (range.minimum), std::abs (range.maximum) });
    }
    return level;
}

// For each envelope point below `points`, the index of the top Clip of the Track at the point's
// first sample, or -1: Clips painted bottom (first) to top (last).
std::vector<int> topClipPerPoint (const song::Track& track, SampleCount points)
{
    std::vector<int> top ((std::size_t) points, -1);
    for (std::size_t i = 0; i < track.clips.size(); ++i)
    {
        const auto& clip = track.clips[i];
        const auto first = std::clamp<SampleCount> (pointsCovering (clip.timelineStart), 0, points);
        const auto last = std::clamp<SampleCount> (pointsCovering (clip.timelineStart + clip.length), 0, points);
        std::fill (top.begin() + first, top.begin() + last, (int) i);
    }
    return top;
}

std::string tenthsText (SampleCount tenths)
{
    return std::format ("{}.{} s", tenths / 10, tenths % 10);
}
} // namespace

LookAheadWindow lookAheadWindow (SampleCount playhead, int sampleRate)
{
    const auto length = (SampleCount) lookAheadWindowSeconds * std::max (sampleRate, 1);
    const auto past = (SampleCount) std::llround ((double) length * lookAheadPlayheadFraction);
    return { playhead - past, length };
}

double lookAheadX (const LookAheadWindow& window, SampleCount sample, int width)
{
    if (window.length <= 0)
        return 0.0;
    return (double) (sample - window.start) * width / (double) window.length;
}

BackingEnvelope backingEnvelope (const song::Song& song, const PeaksLookup& peaksFor)
{
    SampleCount end = 0;
    for (const auto& track : song.tracks)
        for (const auto& clip : track.clips)
            if (clip.source.kind == song::ClipSourceKind::backing)
                end = std::max (end, clip.timelineStart + clip.length);

    BackingEnvelope envelope;
    const auto points = pointsCovering (end);
    envelope.levels.assign ((std::size_t) points, 0.0f);

    const auto anySoloed = std::ranges::any_of (song.tracks, &song::Track::solo);
    for (const auto& track : song.tracks)
    {
        if (! engine::isAudible (track, anySoloed))
            continue;
        const auto volume = (float) core::decibelsToGain (track.volumeDb);
        const auto top = topClipPerPoint (track, points);
        for (SampleCount point = 0; point < points; ++point)
        {
            const auto clipIndex = top[(std::size_t) point];
            if (clipIndex < 0)
                continue;
            const auto& clip = track.clips[(std::size_t) clipIndex];
            if (clip.source.kind != song::ClipSourceKind::backing)
                continue;

            const auto sample = point * song::samplesPerPeak;
            const auto sourceSample = sample - clip.timelineStart + clip.sourceOffset;
            const auto clipEnvelope = engine::ClipEnvelope { clip.timelineStart, clip.length, core::decibelsToGain (clip.gainDb), clip.fadeInLength, clip.fadeOutLength };
            const auto level = peakLevelAt (peaksFor (clip.source), sourceSample / song::samplesPerPeak)
                             * (float) engine::envelopeAt (clipEnvelope, sample) * volume;
            auto& combined = envelope.levels[(std::size_t) point];
            combined = std::max (combined, level);
        }
    }
    return envelope;
}

float envelopeLevelAt (const BackingEnvelope& envelope, SampleCount sample)
{
    if (sample < 0)
        return 0.0f;
    const auto point = sample / song::samplesPerPeak;
    return point < (SampleCount) envelope.levels.size() ? envelope.levels[(std::size_t) point] : 0.0f;
}

std::vector<float> lookAheadColumns (const BackingEnvelope& envelope, const LookAheadWindow& window, int columns)
{
    std::vector<float> levels ((std::size_t) std::max (columns, 0), 0.0f);
    const auto size = (SampleCount) envelope.levels.size();
    for (int column = 0; column < columns; ++column)
    {
        const auto from = window.start + window.length * column / columns;
        const auto to = window.start + window.length * (column + 1) / columns;
        if (to <= 0)
            continue;
        const auto firstPoint = std::max<SampleCount> (0, from) / song::samplesPerPeak;
        const auto lastPoint = std::min (size, pointsCovering (std::max (to, from + 1)));
        for (auto point = firstPoint; point < lastPoint; ++point)
            levels[(std::size_t) column] = std::max (levels[(std::size_t) column], envelope.levels[(std::size_t) point]);
    }
    return levels;
}

MusicCountdown musicCountdown (const BackingEnvelope& envelope, SampleCount playhead, int sampleRate)
{
    MusicCountdown countdown;
    countdown.soundingNow = envelopeLevelAt (envelope, playhead) > silenceLevel;

    const auto window = lookAheadWindow (playhead, sampleRate);
    const auto horizon = window.start + window.length;
    for (auto point = std::max<SampleCount> (0, playhead) / song::samplesPerPeak + 1; point * song::samplesPerPeak <= horizon; ++point)
    {
        const auto sample = point * song::samplesPerPeak;
        if ((envelopeLevelAt (envelope, sample) > silenceLevel) != countdown.soundingNow)
        {
            countdown.changeIn = sample - playhead;
            break;
        }
    }
    return countdown;
}

std::string musicCountdownText (const MusicCountdown& countdown, int sampleRate)
{
    if (! countdown.changeIn.has_value() || sampleRate <= 0)
        return countdown.soundingNow ? "Music continues" : "Silence";
    const auto tenths = *countdown.changeIn * 10 / sampleRate;
    return (countdown.soundingNow ? "Silence in " : "Music back in ") + tenthsText (tenths);
}

LookAheadView lookAheadView (const BackingEnvelope& envelope, const RecordTransport& transport, int sampleRate, int columns)
{
    LookAheadView view;
    view.window = lookAheadWindow (transport.playhead, sampleRate);
    view.columns = lookAheadColumns (envelope, view.window, columns);
    view.passStart = transport.passStart;
    view.recordPoint = transport.recordPoint;
    view.countdown = musicCountdownText (musicCountdown (envelope, transport.playhead, sampleRate), sampleRate);
    if (transport.recordPoint.has_value())
        view.preRollText = "Pass starts in " + preRollCountdownText (*transport.recordPoint - transport.playhead, sampleRate);
    return view;
}

SampleCount overviewLength (const song::Song& song, SampleCount playhead)
{
    auto length = std::max ((SampleCount) lookAheadWindowSeconds * song.sampleRate, playhead);
    for (const auto& track : song.tracks)
        for (const auto& clip : track.clips)
            length = std::max (length, clip.timelineStart + clip.length);
    return length;
}

SampleCount overviewSampleAt (int x, int width, SampleCount length)
{
    if (width <= 0)
        return 0;
    return (SampleCount) std::clamp (x, 0, width) * length / width;
}

double overviewX (SampleCount sample, int width, SampleCount length)
{
    if (length <= 0)
        return 0.0;
    return (double) sample * width / (double) length;
}
} // namespace overdub::ui
