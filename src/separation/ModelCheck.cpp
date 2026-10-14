#include "separation/ModelCheck.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <optional>

namespace overdub::separation
{
namespace
{
std::string shapeText (const std::vector<std::int64_t>& shape)
{
    return nlohmann::json (shape).dump();
}

core::Error modelMismatch (const std::string& path, const std::string& key, const std::string& expected, const std::string& actual)
{
    return core::Error {
        "separation.model_mismatch",
        "the model is not the BS-RoFormer SW graph the helper runs (" + key + ")",
        { { "path", path }, { "key", key }, { "expected", expected }, { "actual", actual } },
    };
}

std::string lowercase (std::string text)
{
    std::transform (text.begin(), text.end(), text.begin(), [] (unsigned char c) { return (char) std::tolower (c); });
    return text;
}

// The metadata value in a canonical text form, so "False" and "false" or "[\"a\", \"b\"]" and
// "[\"a\",\"b\"]" compare equal, as install_model.py compares them.
std::optional<std::string> canonicalValue (const std::string& key, const std::string& raw)
{
    if (key == "sources")
    {
        const auto parsed = nlohmann::json::parse (raw, nullptr, false);
        if (parsed.is_discarded())
            return std::nullopt;
        return parsed.dump();
    }
    if (key == "stft_normalized")
        return lowercase (raw);
    return raw;
}

struct ExpectedValue
{
    std::string key;
    std::string value;
};

std::vector<ExpectedValue> expectedMetadata()
{
    const auto shape = expectedGraphShape();
    return {
        { "architecture", "bs_roformer" },
        { "sources", nlohmann::json (expectedParts).dump() },
        { "segment_samples", std::to_string (shape.segment) },
        { "sample_rate", std::to_string (modelSampleRate) },
        { "stft_n_fft", std::to_string (shape.stft.fftSize) },
        { "stft_hop_length", std::to_string (shape.stft.hop) },
        { "stft_win_length", std::to_string (shape.stft.fftSize) },
        { "stft_normalized", shape.stft.normalized ? "true" : "false" },
        { "stft_window", "hann" },
    };
}

std::optional<core::Error> checkTensor (const std::map<std::string, std::vector<std::int64_t>>& shapes,
                                        const std::string& name,
                                        const std::vector<std::int64_t>& expected,
                                        const std::string& path)
{
    const auto found = shapes.find (name);
    if (found == shapes.end())
        return modelMismatch (path, name, shapeText (expected), "missing");
    if (found->second != expected)
        return modelMismatch (path, name, shapeText (expected), shapeText (found->second));
    return std::nullopt;
}
} // namespace

GraphShape expectedGraphShape()
{
    return GraphShape { .parts = (int) expectedParts.size(), .channels = 2, .segment = 588800, .stft = StftConfig { .fftSize = 2048, .hop = 512, .normalized = false } };
}

core::Expected<GraphShape> checkModelFacts (const ModelFacts& facts, const std::string& path)
{
    for (const auto& expected : expectedMetadata())
    {
        const auto found = facts.metadata.find (expected.key);
        if (found == facts.metadata.end())
            return std::unexpected (modelMismatch (path, expected.key, expected.value, "missing"));
        if (canonicalValue (expected.key, found->second) != expected.value)
            return std::unexpected (modelMismatch (path, expected.key, expected.value, found->second));
    }

    const auto shape = expectedGraphShape();
    const std::vector<std::int64_t> inputShape { 1, shape.channels, shape.stft.bins(), shape.stft.framesFor (shape.segment) };
    const std::vector<std::int64_t> outputShape { 1, shape.parts, shape.channels, shape.stft.bins(), shape.stft.framesFor (shape.segment) };
    for (const auto* name : { "spec_real", "spec_imag" })
        if (auto problem = checkTensor (facts.inputShapes, name, inputShape, path))
            return std::unexpected (*problem);
    for (const auto* name : { "out_spec_real", "out_spec_imag" })
        if (auto problem = checkTensor (facts.outputShapes, name, outputShape, path))
            return std::unexpected (*problem);
    return shape;
}
} // namespace overdub::separation
