#pragma once

#include "InferenceBackend.h"
#include <functional>
#include <mutex>

namespace ve::ml
{
    // A model loaded once on first use and shared by every background job. `run` is
    // serialised, so jobs on several worker threads can share one session.
    class SharedModel
    {
    public:
        // `loader` loads the model into a fresh backend; it returns an error message or "".
        using Loader = std::function<juce::String (InferenceBackend&)>;

        explicit SharedModel (Loader l, BackendOptions o = {}) : loader (std::move (l)), options (o) {}

        bool isLoaded (juce::String* why = nullptr)
        {
            std::lock_guard<std::mutex> g (lock);
            ensureLoaded();
            if (why != nullptr) *why = error;
            return backend != nullptr;
        }

        juce::String getDevice()
        {
            std::lock_guard<std::mutex> g (lock);
            return backend != nullptr ? backend->getDevice() : juce::String();
        }

        juce::Result run (const std::vector<Tensor>& in, std::vector<Tensor>& out)
        {
            std::lock_guard<std::mutex> g (lock);
            ensureLoaded();
            if (backend == nullptr) return juce::Result::fail (error);
            return backend->run (in, out);
        }

        std::vector<TensorInfo> getInputs()
        {
            std::lock_guard<std::mutex> g (lock);
            ensureLoaded();
            return backend != nullptr ? backend->getInputs() : std::vector<TensorInfo> {};
        }

        // Forget the model so the next call looks for it again (e.g. after the user installs it).
        void reset()
        {
            std::lock_guard<std::mutex> g (lock);
            backend.reset();
            tried = false;
            error = {};
        }

    private:
        void ensureLoaded()
        {
            if (tried) return;
            tried = true;
            auto b = createOnnxBackend (options);
            if (b == nullptr) { error = onnxUnavailableReason(); return; }
            error = loader (*b);
            if (error.isEmpty()) backend = std::move (b);
        }

        Loader loader;
        BackendOptions options;
        std::mutex lock;
        std::unique_ptr<InferenceBackend> backend;
        juce::String error;
        bool tried = false;
    };
}
