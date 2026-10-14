#include "separation/GraphSession.h"

#include <onnxruntime_cxx_api.h>

#include <array>
#include <cstdint>
#include <thread>
#include <vector>

namespace overdub::separation
{
struct GraphSession::State
{
    // Fatal-only: ONNX Runtime's own warnings would break the helper's one-line stderr protocol.
    Ort::Env env { ORT_LOGGING_LEVEL_FATAL, "overdub-separate" };
    Ort::Session session { nullptr };
    std::string path;
};

namespace
{
core::Error unloadable (const std::string& path, const std::string& detail)
{
    return core::Error {
        "separation.model_unloadable",
        "ONNX Runtime could not load the model",
        { { "path", path }, { "detail", detail } },
    };
}

std::vector<std::int64_t> shapeOf (const Ort::TypeInfo& info)
{
    return info.GetTensorTypeAndShapeInfo().GetShape();
}
} // namespace

GraphSession::GraphSession (std::unique_ptr<State> sessionState)
    : state (std::move (sessionState))
{
}

GraphSession::~GraphSession() = default;

core::Expected<std::unique_ptr<GraphSession>> GraphSession::open (const std::string& path)
{
    try
    {
        auto sessionState = std::make_unique<State>();
        sessionState->path = path;
        Ort::SessionOptions options;
        options.SetIntraOpNumThreads ((int) std::max (1u, std::thread::hardware_concurrency()));
        options.SetGraphOptimizationLevel (GraphOptimizationLevel::ORT_ENABLE_ALL);
        sessionState->session = Ort::Session (sessionState->env, path.c_str(), options);
        return std::unique_ptr<GraphSession> (new GraphSession (std::move (sessionState)));
    }
    catch (const Ort::Exception& error)
    {
        return std::unexpected (unloadable (path, error.what()));
    }
}

core::Expected<ModelFacts> GraphSession::facts() const
{
    try
    {
        Ort::AllocatorWithDefaultOptions allocator;
        ModelFacts facts;
        const auto metadata = state->session.GetModelMetadata();
        for (const auto& key : metadata.GetCustomMetadataMapKeysAllocated (allocator))
            facts.metadata[key.get()] = metadata.LookupCustomMetadataMapAllocated (key.get(), allocator).get();
        for (std::size_t i = 0; i < state->session.GetInputCount(); ++i)
            facts.inputShapes[state->session.GetInputNameAllocated (i, allocator).get()] = shapeOf (state->session.GetInputTypeInfo (i));
        for (std::size_t i = 0; i < state->session.GetOutputCount(); ++i)
            facts.outputShapes[state->session.GetOutputNameAllocated (i, allocator).get()] = shapeOf (state->session.GetOutputTypeInfo (i));
        return facts;
    }
    catch (const Ort::Exception& error)
    {
        return std::unexpected (unloadable (state->path, error.what()));
    }
}

RunGraph GraphSession::runner (const GraphShape& shape)
{
    const auto bins = (std::int64_t) shape.stft.bins();
    const auto frames = (std::int64_t) shape.stft.framesFor (shape.segment);
    const std::array<std::int64_t, 4> inputShape { 1, shape.channels, bins, frames };
    const std::array<std::int64_t, 5> outputShape { 1, shape.parts, shape.channels, bins, frames };

    return [this, inputShape, outputShape] (std::span<const float> specReal, std::span<const float> specImag, std::span<float> outReal, std::span<float> outImag) -> core::Expected<void> {
        try
        {
            const auto memory = Ort::MemoryInfo::CreateCpu (OrtArenaAllocator, OrtMemTypeDefault);
            // ONNX Runtime only reads input tensors; CreateTensor takes a non-const pointer.
            std::array<Ort::Value, 2> inputs {
                Ort::Value::CreateTensor<float> (memory, const_cast<float*> (specReal.data()), specReal.size(), inputShape.data(), inputShape.size()),
                Ort::Value::CreateTensor<float> (memory, const_cast<float*> (specImag.data()), specImag.size(), inputShape.data(), inputShape.size()),
            };
            std::array<Ort::Value, 2> outputs {
                Ort::Value::CreateTensor<float> (memory, outReal.data(), outReal.size(), outputShape.data(), outputShape.size()),
                Ort::Value::CreateTensor<float> (memory, outImag.data(), outImag.size(), outputShape.data(), outputShape.size()),
            };
            const std::array<const char*, 2> inputNames { "spec_real", "spec_imag" };
            const std::array<const char*, 2> outputNames { "out_spec_real", "out_spec_imag" };
            state->session.Run (Ort::RunOptions { nullptr }, inputNames.data(), inputs.data(), inputs.size(), outputNames.data(), outputs.data(), outputs.size());
            return {};
        }
        catch (const Ort::Exception& error)
        {
            return std::unexpected (core::Error { "separation.inference_failed", "ONNX Runtime could not run the graph", { { "detail", error.what() } } });
        }
    };
}
} // namespace overdub::separation
