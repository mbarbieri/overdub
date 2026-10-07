#pragma once

#include "core/Error.h"
#include "song/AtomicWrite.h"
#include "song/Song.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <string>
#include <string_view>
#include <vector>

// The peaks cache (ADR 0004, issue #14 amendment): peaks/<media-id>.peaks holds the min/max of
// every 256 samples of every channel of a Take or Backing Track. It is a cache, never part of
// history: written at import (and by Recording when a Pass ends), rebuilt from the media file,
// logged as song.peaks_rebuilt, whenever it is missing or corrupt.
//
// File format, little-endian:
//   "OVPK"  4 bytes magic
//   u32     format version (1)
//   u32     samples per peak (256)
//   u32     channel count
//   i64     length in samples
//   then per channel, ceil(length / 256) pairs of f32 (min, max), in time order.
// The file size must match exactly; anything else is corrupt.
namespace overdub::song
{
inline constexpr int samplesPerPeak = 256;

struct PeakRange
{
    float minimum = 0.0f;
    float maximum = 0.0f;

    bool operator== (const PeakRange&) const = default;
};

struct Peaks
{
    SampleCount lengthSamples = 0;
    std::vector<std::vector<PeakRange>> channels; // channels[c][i] covers samples [256 i, 256 (i + 1))

    bool operator== (const Peaks&) const = default;
};

// The min/max of each 256-sample block of each channel; the last block may be shorter.
Peaks computePeaks (const juce::AudioBuffer<float>& audio);

std::string encodePeaks (const Peaks& peaks);

// Errors: song.peaks_corrupt (reason).
core::Expected<Peaks> decodePeaks (std::string_view bytes);

// peaks/<mediaId>.peaks under the package root.
juce::File peaksFileFor (const juce::File& packageRoot, const Id& mediaId);

// The one writer, used at import and at Pass end: computes the peaks of `audio` and writes them
// atomically to `peaksFile`, creating its folder. Errors: song.write_failed, or the hook's Error.
core::Expected<void> writePeaksFile (const juce::File& peaksFile, const juce::AudioBuffer<float>& audio, const BeforeRenameHook& beforeRename);

// Reads peaks/<mediaId>.peaks. When it is missing or corrupt, rebuilds it from `mediaFile` (a WAV),
// logs song.peaks_rebuilt (info: media_id, reason missing|corrupt, path) and returns the new peaks.
// Errors: song.read_failed (the media file is missing or unreadable), song.write_failed.
core::Expected<Peaks> loadOrRebuildPeaks (const juce::File& packageRoot, const Id& mediaId, const juce::File& mediaFile, const BeforeRenameHook& beforeRename);
} // namespace overdub::song
