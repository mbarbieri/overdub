#pragma once

#include "core/Error.h"
#include "separation/SeparationPipeline.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

// What the helper checks about a Separation model before running it (ADR 0007): its ONNX metadata
// and its tensors, never a file hash (an export is not byte-reproducible). The expected values are
// those of scripts/separation-model/install_model.py (EXPECTED_METADATA, EXPECTED_INPUT_SHAPES).
namespace overdub::separation
{
// The parts, in the graph's output order and as its `sources` metadata names them.
inline const std::vector<std::string> expectedParts { "bass", "drums", "other", "vocals", "guitar", "piano" };
inline constexpr int modelSampleRate = 44100;

// What an ONNX file says about itself.
struct ModelFacts
{
    std::map<std::string, std::string> metadata;                  // custom metadata_props
    std::map<std::string, std::vector<std::int64_t>> inputShapes;  // by tensor name
    std::map<std::string, std::vector<std::int64_t>> outputShapes; // by tensor name
};

// The graph BS-RoFormer SW's export must be: 6 parts, stereo, 588800-sample chunks, STFT 2048/512.
// The one place these numbers are written down in C++.
GraphShape expectedGraphShape();

// The graph's tensors: [1, channels, bins, frames] in, [1, parts, channels, bins, frames] out.
inline constexpr const char* inputRealName = "spec_real";
inline constexpr const char* inputImagName = "spec_imag";
inline constexpr const char* outputRealName = "out_spec_real";
inline constexpr const char* outputImagName = "out_spec_imag";
std::vector<std::int64_t> inputTensorShape (const GraphShape& shape);
std::vector<std::int64_t> outputTensorShape (const GraphShape& shape);

// The shape to run when `facts` describe the expected graph. Errors: separation.model_mismatch
// (path, key: the metadata key or tensor name that differs, expected, actual; actual is "missing"
// when the key or tensor is absent).
core::Expected<GraphShape> checkModelFacts (const ModelFacts& facts, const std::string& path);
} // namespace overdub::separation
