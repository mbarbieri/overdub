#include "separation/CrossFade.h"

#include <algorithm>
#include <cstddef>

namespace overdub::separation
{
int strideFor (int segment)
{
    return (int) (0.75 * segment);
}

int chunkCountFor (int length, int segment)
{
    const int stride = strideFor (segment);
    return (length + stride - 1) / stride;
}

std::vector<float> triangularWeight (int length)
{
    const int half = length / 2;
    std::vector<double> ramp;
    ramp.reserve ((std::size_t) length);
    for (int i = 1; i <= half; ++i)
        ramp.push_back (i);
    for (int i = length - half; i > 0; --i)
        ramp.push_back (i);
    const double peak = *std::max_element (ramp.begin(), ramp.end());
    std::vector<float> weight ((std::size_t) length);
    for (std::size_t i = 0; i < weight.size(); ++i)
        weight[i] = (float) std::max (ramp[i] / peak, 1e-20);
    return weight;
}

CrossFade::CrossFade (int segment, int inputLength)
    : length (inputLength),
      weight (triangularWeight (segment)),
      weightSum ((std::size_t) inputLength, 0.0f)
{
}

int CrossFade::samplesInside (int offset) const
{
    return std::clamp (length - offset, 0, (int) weight.size());
}

void CrossFade::addChunk (std::span<float> destination, int offset, std::span<const float> chunk) const
{
    const int count = samplesInside (offset);
    for (int i = 0; i < count; ++i)
        destination[(std::size_t) (offset + i)] += chunk[(std::size_t) i] * weight[(std::size_t) i];
}

void CrossFade::addChunkWeight (int offset)
{
    const int count = samplesInside (offset);
    for (int i = 0; i < count; ++i)
        weightSum[(std::size_t) (offset + i)] += weight[(std::size_t) i];
}

void CrossFade::finish (std::span<float> destination) const
{
    for (std::size_t i = 0; i < destination.size(); ++i)
        destination[i] /= weightSum[i];
}
} // namespace overdub::separation
