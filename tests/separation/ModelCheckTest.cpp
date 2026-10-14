#include "separation/ModelCheck.h"

#include "OnnxTestModel.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cstdint>
#include <string>
#include <vector>

using overdub::separation::checkModelFacts;
using overdub::separation::ModelFacts;
using namespace overdub::separation::testing;

namespace
{
const std::vector<std::int64_t> inputShape { 1, 2, 1025, 1151 };
const std::vector<std::int64_t> outputShape { 1, 6, 2, 1025, 1151 };

// What the real BS-RoFormer SW export says about itself (scripts/separation-model/install_model.py).
ModelFacts validFacts()
{
    return ModelFacts {
        .metadata = separationModelMetadata(),
        .inputShapes = { { "spec_real", inputShape }, { "spec_imag", inputShape } },
        .outputShapes = { { "out_spec_real", outputShape }, { "out_spec_imag", outputShape } },
    };
}

void requireMismatch (const ModelFacts& facts, const std::string& key)
{
    const auto checked = checkModelFacts (facts, "/models/m.onnx");
    REQUIRE_FALSE (checked.has_value());
    CHECK (checked.error().code == "separation.model_mismatch");
    CHECK (checked.error().fields["key"] == key);
    CHECK (checked.error().fields["path"] == "/models/m.onnx");
}
} // namespace

TEST_CASE ("The BS-RoFormer SW export's metadata gives the graph shape the helper runs", "[separation][model]")
{
    const auto checked = checkModelFacts (validFacts(), "/models/m.onnx");
    REQUIRE (checked.has_value());
    CHECK (checked->parts == 6);
    CHECK (checked->channels == 2);
    CHECK (checked->segment == 588800);
    CHECK (checked->stft.fftSize == 2048);
    CHECK (checked->stft.hop == 512);
    CHECK_FALSE (checked->stft.normalized);
    CHECK (checked->stft.framesFor (checked->segment) == 1151);
}

TEST_CASE ("Metadata written in another style still matches", "[separation][model]")
{
    auto facts = validFacts();
    facts.metadata["stft_normalized"] = "False";
    facts.metadata["sources"] = R"([ "bass", "drums", "other", "vocals", "guitar", "piano" ])";
    facts.metadata["external_normalization"] = "false"; // keys the helper does not use are ignored
    CHECK (checkModelFacts (facts, "/models/m.onnx").has_value());
}

TEST_CASE ("Metadata that does not describe the expected graph is a model mismatch", "[separation][model]")
{
    const auto [key, value] = GENERATE (table<std::string, std::string> ({
        { "architecture", "htdemucs" },
        { "sources", R"(["vocals","drums","bass","other"])" },
        { "sources", "not json" },
        { "segment_samples", "409600" },
        { "sample_rate", "48000" },
        { "stft_n_fft", "4096" },
        { "stft_hop_length", "1024" },
        { "stft_win_length", "4096" },
        { "stft_normalized", "true" },
        { "stft_window", "hamming" },
    }));
    auto facts = validFacts();
    facts.metadata[key] = value;
    requireMismatch (facts, key);
}

TEST_CASE ("A missing metadata key is a model mismatch", "[separation][model]")
{
    const auto key = GENERATE (as<std::string> {}, "architecture", "sources", "segment_samples", "sample_rate", "stft_n_fft", "stft_hop_length", "stft_win_length", "stft_normalized", "stft_window");
    auto facts = validFacts();
    facts.metadata.erase (key);
    requireMismatch (facts, key);
    CHECK (checkModelFacts (facts, "/m.onnx").error().fields["actual"] == "missing");
}

TEST_CASE ("Graph tensors of another name or shape are a model mismatch", "[separation][model]")
{
    SECTION ("an input of another shape")
    {
        auto facts = validFacts();
        facts.inputShapes["spec_imag"] = { 1, 2, 1025, 801 };
        requireMismatch (facts, "spec_imag");
    }
    SECTION ("a missing input")
    {
        auto facts = validFacts();
        facts.inputShapes.erase ("spec_real");
        facts.inputShapes["mix"] = inputShape;
        requireMismatch (facts, "spec_real");
    }
    SECTION ("an output with fewer parts")
    {
        auto facts = validFacts();
        facts.outputShapes["out_spec_real"] = { 1, 4, 2, 1025, 1151 };
        requireMismatch (facts, "out_spec_real");
    }
    SECTION ("a missing output")
    {
        auto facts = validFacts();
        facts.outputShapes.erase ("out_spec_imag");
        requireMismatch (facts, "out_spec_imag");
    }
}
