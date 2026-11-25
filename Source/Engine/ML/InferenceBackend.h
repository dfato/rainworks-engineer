#pragma once

#include <juce_core/juce_core.h>
#include <cstdint>
#include <memory>
#include <vector>

namespace ve::ml
{
    // A named float tensor. Enough for the smoke test; later stages can add
    // other element types without changing callers that only use float.
    struct Tensor
    {
        juce::String name;
        std::vector<std::int64_t> shape;
        std::vector<float> data;
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
    };

    // True when the build has ONNX Runtime (CMake option VE_WITH_ONNX).
    bool isOnnxAvailable() noexcept;

    // Returns nullptr when built without ONNX Runtime.
    std::unique_ptr<InferenceBackend> createOnnxBackend();

    // A tiny ONNX model (Y = X + X, X float[4]) generated in code, for smoke
    // tests only. No model files ship with the plugin.
    juce::MemoryBlock makeSmokeTestModel();
}
