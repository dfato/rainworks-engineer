#pragma once

#include <juce_core/juce_core.h>
#include <cstdint>
#include <memory>
#include <vector>

namespace ve::ml
{
    // A named tensor: float (`data`) or int64 (`ints`, with type = int64).
    struct Tensor
    {
        enum class Type { float32, int64 };

        juce::String name;
        std::vector<std::int64_t> shape;
        std::vector<float> data;
        std::vector<std::int64_t> ints;
        Type type = Type::float32;

        static Tensor makeFloat (juce::String n, std::vector<std::int64_t> shp, std::vector<float> v)
        {
            Tensor t; t.name = std::move (n); t.shape = std::move (shp); t.data = std::move (v);
            return t;
        }
        static Tensor makeInt64 (juce::String n, std::vector<std::int64_t> shp, std::vector<std::int64_t> v)
        {
            Tensor t; t.name = std::move (n); t.shape = std::move (shp); t.ints = std::move (v); t.type = Type::int64;
            return t;
        }
        size_t elementCount() const noexcept { return type == Type::int64 ? ints.size() : data.size(); }
    };

    // Name and rank of a model input or output (rank -1 = unknown).
    struct TensorInfo
    {
        juce::String name;
        int rank = -1;
    };

    // Inference backend interface. Engine code depends only on this, so the
    // Stage 5 renderer can run in-process (ONNX Runtime) or in a sidecar
    // process without touching callers. Never call from the audio thread.
    class InferenceBackend
    {
    public:
        virtual ~InferenceBackend() = default;

        virtual juce::String getName() const = 0;
        virtual juce::Result loadModel (const juce::File& modelFile) = 0;
        virtual juce::Result loadModel (const void* data, size_t size) = 0;
        virtual juce::Result run (const std::vector<Tensor>& inputs, std::vector<Tensor>& outputs) = 0;

        virtual std::vector<TensorInfo> getInputs() const = 0;
        virtual std::vector<TensorInfo> getOutputs() const = 0;
    };

    struct BackendOptions
    {
        int threads = 1;  // intra-op threads
    };

    // True when the build has ONNX Runtime (CMake option VE_WITH_ONNX) and the runtime
    // library could be loaded (on Windows it's loaded from next to the plugin binary).
    bool isOnnxAvailable() noexcept;
    // Why it isn't available (empty when it is).
    juce::String onnxUnavailableReason();

    // Returns nullptr when ONNX Runtime isn't available.
    std::unique_ptr<InferenceBackend> createOnnxBackend (BackendOptions = {});

    // A tiny ONNX model (Y = X + X, X float[4]) generated in code, for smoke
    // tests only. No model files ship with the plugin.
    juce::MemoryBlock makeSmokeTestModel();
}
