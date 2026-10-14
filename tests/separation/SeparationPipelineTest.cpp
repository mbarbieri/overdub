#include "separation/SeparationPipeline.h"

#include "separation/CrossFade.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <utility>
#include <vector>

using overdub::core::Error;
using overdub::core::Expected;
using overdub::separation::GraphShape;
using overdub::separation::RunGraph;
using overdub::separation::separateAudio;
using overdub::separation::strideFor;

namespace
{
// A small graph shape so a test runs many chunks quickly; the pipeline is the same at any size.
GraphShape smallShape()
{
    return GraphShape { .parts = 6, .channels = 2, .segment = 8192, .stft = { .fftSize = 256, .hop = 64, .normalized = false } };
}

// A graph that gives every part the input's spectrum: each part must come back as the input.
RunGraph identityGraph (const GraphShape& shape)
{
    return [shape] (std::span<const float> specReal, std::span<const float> specImag, std::span<float> outReal, std::span<float> outImag) -> Expected<void> {
        for (int p = 0; p < shape.parts; ++p)
        {
            std::copy (specReal.begin(), specReal.end(), outReal.begin() + (std::ptrdiff_t) ((std::size_t) p * specReal.size()));
            std::copy (specImag.begin(), specImag.end(), outImag.begin() + (std::ptrdiff_t) ((std::size_t) p * specImag.size()));
        }
        return {};
    };
}

// Left: a 440 Hz sine; right: a 1 kHz sine at another level.
juce::AudioBuffer<float> stereoSines (int length)
{
    juce::AudioBuffer<float> audio (2, length);
    for (int i = 0; i < length; ++i)
    {
        audio.setSample (0, i, 0.5f * (float) std::sin (2.0 * std::numbers::pi * 440.0 * i / 44100.0));
        audio.setSample (1, i, 0.25f * (float) std::sin (2.0 * std::numbers::pi * 1000.0 * i / 44100.0));
    }
    return audio;
}

float worstDifference (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
{
    float worst = 0.0f;
    for (int c = 0; c < a.getNumChannels(); ++c)
        for (int i = 0; i < a.getNumSamples(); ++i)
            worst = std::max (worst, std::abs (a.getSample (c, i) - b.getSample (c, i)));
    return worst;
}

struct Run
{
    std::vector<juce::AudioBuffer<float>> parts;
    std::vector<std::pair<int, int>> reported;
};

Run separate (const juce::AudioBuffer<float>& input, const GraphShape& shape)
{
    Run run;
    auto parts = separateAudio (input, shape, identityGraph (shape), [&] (int chunk, int chunks) { run.reported.emplace_back (chunk, chunks); });
    REQUIRE (parts.has_value());
    run.parts = std::move (*parts);
    return run;
}

void requirePartsEqualInput (const Run& run, const juce::AudioBuffer<float>& input, const GraphShape& shape)
{
    REQUIRE ((int) run.parts.size() == shape.parts);
    for (const auto& part : run.parts)
    {
        REQUIRE (part.getNumChannels() == 2);
        REQUIRE (part.getNumSamples() == input.getNumSamples());
        CHECK (worstDifference (part, input) < 1e-5f);
    }
}
} // namespace

TEST_CASE ("An input shorter than one segment comes out at its own length", "[separation][pipeline]")
{
    const auto shape = smallShape();
    const auto input = stereoSines (3001);
    const auto run = separate (input, shape);

    requirePartsEqualInput (run, input, shape);
    CHECK (run.reported == std::vector<std::pair<int, int>> { { 1, 1 } });
}

TEST_CASE ("An input of an exact multiple of the stride comes out at its own length", "[separation][pipeline]")
{
    const auto shape = smallShape();
    const int stride = strideFor (shape.segment);
    const auto input = stereoSines (3 * stride);
    const auto run = separate (input, shape);

    requirePartsEqualInput (run, input, shape);
    CHECK (run.reported == std::vector<std::pair<int, int>> { { 1, 3 }, { 2, 3 }, { 3, 3 } });
}

TEST_CASE ("Overlapping chunks cross-fade back into the input", "[separation][pipeline]")
{
    const auto shape = smallShape();
    const auto input = stereoSines (5 * strideFor (shape.segment) + 777);
    const auto run = separate (input, shape);

    requirePartsEqualInput (run, input, shape);
    CHECK (run.reported.size() == 6);
}

TEST_CASE ("A graph failure stops the Separation with the graph's Error", "[separation][pipeline]")
{
    const auto shape = smallShape();
    int calls = 0;
    const RunGraph failing = [&] (std::span<const float>, std::span<const float>, std::span<float>, std::span<float>) -> Expected<void> {
        ++calls;
        return std::unexpected (Error { "separation.inference_failed", "test", { { "detail", "boom" } } });
    };
    const auto parts = separateAudio (stereoSines (20000), shape, failing, {});

    REQUIRE_FALSE (parts.has_value());
    CHECK (parts.error().code == "separation.inference_failed");
    CHECK (calls == 1);
}
