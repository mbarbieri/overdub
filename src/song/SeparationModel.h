#pragma once

#include "core/Error.h"

#include <juce_core/juce_core.h>

#include <map>
#include <string>

// What the app checks about the installed Separation model before it starts the helper (issue
// #115, ADR 0007): the file exists and its ONNX metadata names the graph the app maps to its six
// parts (`architecture` bs_roformer, `sources` the separationParts file stems in order). The
// metadata is read straight from the file's top-level protobuf fields, so the app needs no ONNX
// library (ONNX Runtime links only into the helper). The helper checks the rest of the graph
// (STFT keys, tensors) itself and fails with separation.model_mismatch when it differs.
namespace overdub::song
{
// The ONNX ModelProto's metadata_props (field 14) as key -> value. Skips every other top-level
// field by its length, so a large graph is not read.
// Errors: song.separation_model_invalid (path, reason "unreadable" or "not_onnx").
core::Expected<std::map<std::string, std::string>> readOnnxMetadata (const juce::File& model);

// Errors: song.separation_model_missing (path), song.separation_model_invalid (path, and either
// reason "unreadable"/"not_onnx", or key, expected, actual; actual is "missing" when the key is
// absent).
core::Expected<void> checkSeparationModel (const juce::File& model);
} // namespace overdub::song
