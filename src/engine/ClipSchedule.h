#pragma once

#include "song/Song.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <memory>
#include <span>
#include <vector>

// A Track's Clips flattened for playback (ADR 0004 layer order): where Clips overlap only the top
// one sounds, so the schedule is a sorted list of non-overlapping segments, each read verbatim
// from one source. Built off the audio thread; rendered on it without allocating.
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
    SourceAudio source;
};

// Timeline range [timelineStart, timelineEnd) plays the source from sourceStart onwards.
struct ClipSegment
{
    SampleCount timelineStart = 0;
    SampleCount timelineEnd = 0;
    SampleCount sourceStart = 0;
    const juce::AudioBuffer<float>* source = nullptr; // null = silence that still hides lower Clips
};

// Paints Clips bottom (first) to top (last); returns segments sorted by timelineStart.
std::vector<ClipSegment> flattenClips (std::span<const LayeredClip> clipsInLayerOrder);

// Writes song samples [startSample, startSample + numSamples) of the schedule into every channel of
// `out` (cleared first). A mono source feeds every channel; a stereo source feeds channel c from
// source channel min(c, 1). Source samples outside the file are silence. Never allocates.
void renderSegments (std::span<const ClipSegment> segments, SampleCount startSample, juce::AudioBuffer<float>& out) noexcept;
} // namespace overdub::engine
