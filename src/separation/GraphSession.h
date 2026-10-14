#pragma once

#include "core/Error.h"
#include "separation/ModelCheck.h"
#include "separation/SeparationPipeline.h"

#include <memory>
#include <string>

// The Separation graph on ONNX Runtime (ADR 0007). Only the overdub-separate helper compiles this
// file: ONNX Runtime never links into overdub_core, overdub_separation or Overdub.app.
namespace overdub::separation
{
class GraphSession
{
public:
    // Loads the model with every core for ONNX Runtime's intra-op threads.
    // Errors: separation.model_unloadable (path, detail).
    static core::Expected<std::unique_ptr<GraphSession>> open (const std::string& path);

    ~GraphSession();

    // The model's metadata and tensor shapes, for checkModelFacts.
    core::Expected<ModelFacts> facts() const;

    // Runs the graph for separateAudio. `shape` must be the one checkModelFacts returned.
    // Errors: separation.inference_failed (detail).
    RunGraph runner (const GraphShape& shape);

private:
    struct State;
    explicit GraphSession (std::unique_ptr<State> state);
    std::unique_ptr<State> state;
};
} // namespace overdub::separation
