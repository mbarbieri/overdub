#pragma once

#include "song/Song.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <memory>
#include <span>
#include <vector>

// A Track's Clips flattened for playback (ADR 0004 layer order): where Clips overlap only the top
// one sounds, so the schedule is a sorted list of non-overlapping segments, each read from one
// source through its Clip's envelope. Built off the audio thread; rendered on it without allocating.
//
// The Clip envelope (issue #29; the one fade curve in v1, linear in amplitude), for sample k of a
// Clip of length L: gain = 10^(gainDb / 20), times k / fadeIn while k < fadeIn, times
// (L - 1 - k) / fadeOut while L - 1 - k < fadeOut. A fade starts or ends at exactly 0.
namespace overdub::engine
{
using song::SampleCount;

// Decoded float audio of a Take or Backing Track at the Song rate. Immutable once loaded.
using SourceAudio = std::shared_ptr<const juce::AudioBuffer<float>>;

// One Clip in layer order with its decoded source (null source = missing file: plays silence).
struct LayeredClip
{
    SampleCount timelineStart = 0;
    SampleCount sourceOffset = 0;
    SampleCount length = 0;
    double gainDb = 0.0;
    SampleCount fadeInLength = 0;
    SampleCount fadeOutLength = 0;
    SourceAudio source;
};

// The whole Clip a segment belongs to, for its envelope: a segment may be any part of it.
struct ClipEnvelope
{
    SampleCount clipStart = 0; // the Clip's timeline start
    SampleCount clipLength = 0;
    double gain = 1.0; // linear
    SampleCount fadeInLength = 0;
    SampleCount fadeOutLength = 0;
};

// The envelope's amplitude at Song sample `timelineSample` (inside the Clip).
double envelopeAt (const ClipEnvelope& envelope, SampleCount timelineSample) noexcept;

// Timeline range [timelineStart, timelineEnd) plays the source from sourceStart onwards.
struct ClipSegment
{
    SampleCount timelineStart = 0;
    SampleCount timelineEnd = 0;
    SampleCount sourceStart = 0;
    const juce::AudioBuffer<float>* source = nullptr; // null = silence that still hides lower Clips
    ClipEnvelope envelope;
};

// Paints Clips bottom (first) to top (last); returns segments sorted by timelineStart.
std::vector<ClipSegment> flattenClips (std::span<const LayeredClip> clipsInLayerOrder);

// Writes song samples [startSample, startSample + numSamples) of the schedule into every channel of
// `out` (cleared first). A mono source feeds every channel; a stereo source feeds channel c from
// source channel min(c, 1), through the segment's Clip envelope. Source samples outside the file
// are silence. Never allocates.
void renderSegments (std::span<const ClipSegment> segments, SampleCount startSample, juce::AudioBuffer<float>& out) noexcept;
} // namespace overdub::engine
