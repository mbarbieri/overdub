#pragma once

#include <span>
#include <vector>

// How the Separation's overlapping chunks are joined (ADR 0007): chunks of `segment` samples every
// strideFor (segment) samples, zero-padded past the end of the input, each weighted by unblend's
// triangular cross-fade and divided at the end by the summed weight.
namespace overdub::separation
{
// int (0.75 * segment), as unblend and the Python reference compute it.
int strideFor (int segment);

// Chunks starting at 0, stride, 2 x stride, ... while the start is before `length`. 0 for an empty input.
int chunkCountFor (int length, int segment);

// unblend's triangular weight: 1..half rising, then falling to 1, divided by its peak, floored at 1e-20.
std::vector<float> triangularWeight (int length);

// The weighted sum of the chunks of one input of `length` samples. One CrossFade serves every part
// and channel: they share the chunk offsets, so the summed weight is the same for all of them.
class CrossFade
{
public:
    CrossFade (int segment, int length);

    // How many of a chunk's samples starting at `offset` fall inside the input.
    int samplesInside (int offset) const;

    // Adds weight[i] x chunk[i] to destination[offset + i] for the samples inside the input.
    // destination.size() is the input length; chunk.size() is the segment.
    void addChunk (std::span<float> destination, int offset, std::span<const float> chunk) const;

    // Counts the chunk at `offset` in the summed weight. Call once per chunk, not per part or channel.
    void addChunkWeight (int offset);

    // Divides `destination` (the input length) by the summed weight.
    void finish (std::span<float> destination) const;

private:
    int length = 0;
    std::vector<float> weight;
    std::vector<float> weightSum;
};
} // namespace overdub::separation
