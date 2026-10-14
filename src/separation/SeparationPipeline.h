#pragma once

#include "core/Error.h"
#include "separation/Stft.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <functional>
#include <span>
#include <vector>

// The signal processing around the Separation graph (ADR 0007): chunking, the STFT of each chunk,
// the graph, the iSTFT of each part and the cross-fade. The graph itself is a RunGraph, so this
// runs without ONNX Runtime in tests.
namespace overdub::separation
{
struct GraphShape
{
    int parts = 6;
    int channels = 2;
    int segment = 588800; // samples per chunk
    StftConfig stft;

    // Floats in one [bin][frame] plane of a chunk's spectrum.
    std::size_t planeSize() const { return (std::size_t) stft.bins() * (std::size_t) stft.framesFor (segment); }
};

// Runs the graph on one chunk. specReal / specImag are [channels][bin][frame] (the graph's
// [1, C, F, T] input); outReal / outImag receive [parts][channels][bin][frame] ([1, S, C, F, T]).
using RunGraph = std::function<core::Expected<void> (std::span<const float> specReal,
                                                     std::span<const float> specImag,
                                                     std::span<float> outReal,
                                                     std::span<float> outImag)>;

// Called after each chunk with its 1-based index and the chunk count.
using ReportChunk = std::function<void (int chunk, int chunks)>;

// Splits `input` (shape.channels channels) into shape.parts buffers of the same length and
// channels. Errors: whatever `runGraph` returns, as it returned it.
core::Expected<std::vector<juce::AudioBuffer<float>>> separateAudio (const juce::AudioBuffer<float>& input,
                                                                     const GraphShape& shape,
                                                                     const RunGraph& runGraph,
                                                                     const ReportChunk& reportChunk);
} // namespace overdub::separation
