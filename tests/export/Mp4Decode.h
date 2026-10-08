#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <vector>

// Reads an exported MP4 back for measurement (issue #38, ADR 0005's export round trip), through
// AVAssetReader, so edit lists and the AAC encoder's priming are applied as a player applies them.
namespace overdub::exporting::testing
{
struct DecodedVideoFrame
{
    double seconds = 0.0;  // the frame's presentation time in the file
    double meanLuma = 0.0; // 0..255
};

// Every frame of the first video track; empty when there is none or it cannot be read.
std::vector<DecodedVideoFrame> decodeVideoFrames (const juce::File& movie);

// The first audio track as float at `sampleRate`, one channel per track channel; empty when there
// is none.
juce::AudioBuffer<float> decodeAudio (const juce::File& movie, int sampleRate);

// The file's duration in seconds (0 when it cannot be read).
double movieDurationSeconds (const juce::File& movie);
} // namespace overdub::exporting::testing
