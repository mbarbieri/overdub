#include "separation/SeparationPipeline.h"

#include "separation/CrossFade.h"

#include <algorithm>
#include <cstddef>

namespace overdub::separation
{
core::Expected<std::vector<juce::AudioBuffer<float>>> separateAudio (const juce::AudioBuffer<float>& input,
                                                                     const GraphShape& shape,
                                                                     const RunGraph& runGraph,
                                                                     const ReportChunk& reportChunk)
{
    const int length = input.getNumSamples();
    const int stride = strideFor (shape.segment);
    const int chunks = chunkCountFor (length, shape.segment);
    const auto window = periodicHann (shape.stft.fftSize);
    const auto plane = shape.planeSize();
    const auto channels = (std::size_t) shape.channels;
    const auto parts = (std::size_t) shape.parts;

    std::vector<juce::AudioBuffer<float>> separated (parts, juce::AudioBuffer<float> (shape.channels, length));
    for (auto& part : separated)
        part.clear();

    CrossFade crossFade (shape.segment, length);
    std::vector<float> chunk ((std::size_t) shape.segment);
    std::vector<float> partChunk ((std::size_t) shape.segment);
    std::vector<float> specReal (channels * plane);
    std::vector<float> specImag (channels * plane);
    std::vector<float> outReal (parts * channels * plane);
    std::vector<float> outImag (parts * channels * plane);

    for (int index = 0; index < chunks; ++index)
    {
        const int offset = index * stride;
        const int inside = crossFade.samplesInside (offset);
        for (std::size_t c = 0; c < channels; ++c)
        {
            std::fill (chunk.begin(), chunk.end(), 0.0f);
            std::copy_n (input.getReadPointer ((int) c, offset), inside, chunk.begin());
            stft (chunk, shape.stft, window, std::span (specReal).subspan (c * plane, plane), std::span (specImag).subspan (c * plane, plane));
        }

        if (auto ran = runGraph (specReal, specImag, outReal, outImag); ! ran.has_value())
            return std::unexpected (ran.error());

        for (std::size_t p = 0; p < parts; ++p)
            for (std::size_t c = 0; c < channels; ++c)
            {
                const auto at = (p * channels + c) * plane;
                istft (std::span (outReal).subspan (at, plane), std::span (outImag).subspan (at, plane), shape.stft, window, partChunk);
                crossFade.addChunk ({ separated[p].getWritePointer ((int) c), (std::size_t) length }, offset, partChunk);
            }
        crossFade.addChunkWeight (offset);

        if (reportChunk)
            reportChunk (index + 1, chunks);
    }

    for (auto& part : separated)
        for (int c = 0; c < shape.channels; ++c)
            crossFade.finish ({ part.getWritePointer (c), (std::size_t) length });
    return separated;
}
} // namespace overdub::separation
