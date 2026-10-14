#pragma once

#include <juce_core/juce_core.h>

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

// A stand-in for the Separation model (ADR 0005: tests never run the real one): a tiny ONNX graph
// with the real model's tensor names and shapes that copies its input spectrum to all six parts,
// so overdub-separate turns any input into six copies of it. Written as raw protobuf
// (onnx.proto's ModelProto), because the tests have no ONNX library.
namespace overdub::separation::testing
{
// The metadata scripts/separation-model/install_model.py expects of the real export.
inline std::map<std::string, std::string> separationModelMetadata()
{
    return {
        { "architecture", "bs_roformer" },
        { "sources", R"(["bass", "drums", "other", "vocals", "guitar", "piano"])" },
        { "segment_samples", "588800" },
        { "sample_rate", "44100" },
        { "stft_n_fft", "2048" },
        { "stft_hop_length", "512" },
        { "stft_win_length", "2048" },
        { "stft_normalized", "false" },
        { "stft_window", "hann" },
        { "external_normalization", "false" },
    };
}

namespace protobuf
{
inline void varint (std::string& out, std::uint64_t value)
{
    while (value >= 0x80)
    {
        out.push_back ((char) ((value & 0x7f) | 0x80));
        value >>= 7;
    }
    out.push_back ((char) value);
}

inline void integer (std::string& out, int field, std::int64_t value)
{
    varint (out, (std::uint64_t) field << 3); // wire type 0
    varint (out, (std::uint64_t) value);
}

// Strings, bytes and embedded messages (wire type 2).
inline void bytes (std::string& out, int field, const std::string& value)
{
    varint (out, ((std::uint64_t) field << 3) | 2);
    varint (out, value.size());
    out += value;
}
} // namespace protobuf

// ValueInfoProto: a float tensor of a fixed shape.
inline std::string floatTensorInfo (const std::string& name, const std::vector<std::int64_t>& shape)
{
    std::string dims;
    for (const auto size : shape)
    {
        std::string dimension;
        protobuf::integer (dimension, 1, size); // Dimension.dim_value
        protobuf::bytes (dims, 1, dimension);   // TensorShapeProto.dim
    }
    std::string tensorType;
    protobuf::integer (tensorType, 1, 1); // elem_type FLOAT
    protobuf::bytes (tensorType, 2, dims); // shape
    std::string type;
    protobuf::bytes (type, 1, tensorType); // TypeProto.tensor_type
    std::string info;
    protobuf::bytes (info, 1, name);
    protobuf::bytes (info, 2, type);
    return info;
}

inline std::string node (const std::string& opType, const std::vector<std::string>& inputs, const std::string& output, const std::string& attributes = {})
{
    std::string proto;
    for (const auto& input : inputs)
        protobuf::bytes (proto, 1, input);
    protobuf::bytes (proto, 2, output);
    protobuf::bytes (proto, 3, output + "_node");
    protobuf::bytes (proto, 4, opType);
    if (! attributes.empty())
        protobuf::bytes (proto, 5, attributes);
    return proto;
}

// The identity Separation graph: out_spec_x = Concat (axis 1) of six Unsqueeze (spec_x, [1]).
inline std::string identitySeparationModel (const std::map<std::string, std::string>& metadata)
{
    const std::vector<std::int64_t> inputShape { 1, 2, 1025, 1151 };
    const std::vector<std::int64_t> outputShape { 1, 6, 2, 1025, 1151 };

    std::string axes; // TensorProto: int64 [1] = { 1 }
    protobuf::integer (axes, 1, 1);  // dims
    protobuf::integer (axes, 2, 7);  // data_type INT64
    protobuf::integer (axes, 7, 1);  // int64_data
    protobuf::bytes (axes, 8, "axes");

    std::string concatAxis; // AttributeProto axis = 1 (type INT)
    protobuf::bytes (concatAxis, 1, "axis");
    protobuf::integer (concatAxis, 3, 1);
    protobuf::integer (concatAxis, 20, 2);

    std::string graph;
    for (const std::string part : { "real", "imag" })
    {
        const auto input = "spec_" + part;
        const auto unsqueezed = "unsqueezed_" + part;
        protobuf::bytes (graph, 1, node ("Unsqueeze", { input, "axes" }, unsqueezed));
        protobuf::bytes (graph, 1, node ("Concat", std::vector<std::string> (6, unsqueezed), "out_spec_" + part, concatAxis));
    }
    protobuf::bytes (graph, 2, "identity_separation");
    protobuf::bytes (graph, 5, axes);
    for (const std::string part : { "real", "imag" })
        protobuf::bytes (graph, 11, floatTensorInfo ("spec_" + part, inputShape));
    for (const std::string part : { "real", "imag" })
        protobuf::bytes (graph, 12, floatTensorInfo ("out_spec_" + part, outputShape));

    std::string opset;
    protobuf::integer (opset, 2, 18);

    std::string model;
    protobuf::integer (model, 1, 8); // ir_version
    protobuf::bytes (model, 2, "overdub-tests");
    protobuf::bytes (model, 7, graph);
    protobuf::bytes (model, 8, opset);
    for (const auto& [key, value] : metadata)
    {
        std::string entry;
        protobuf::bytes (entry, 1, key);
        protobuf::bytes (entry, 2, value);
        protobuf::bytes (model, 14, entry);
    }
    return model;
}

inline bool writeIdentitySeparationModel (const juce::File& file, const std::map<std::string, std::string>& metadata)
{
    const auto bytes = identitySeparationModel (metadata);
    return file.replaceWithData (bytes.data(), bytes.size());
}
} // namespace overdub::separation::testing
