#include "InferenceBackend.h"

#if VE_WITH_ONNX
 // The C++ API is initialised by hand (Ort::InitApi) after the runtime library has been
 // loaded from a known path, so a delay-loaded DLL never resolves to a stray copy
 // (Windows 11 ships an older onnxruntime.dll in System32).
 #define ORT_API_MANUAL_INIT 1
 #include <onnxruntime_cxx_api.h>
 #if JUCE_WINDOWS
  #ifndef NOMINMAX
   #define NOMINMAX
  #endif
  #include <windows.h>
 #endif
#endif

#include <mutex>

namespace ve::ml
{
#if VE_WITH_ONNX
    namespace
    {
        struct RuntimeState
        {
            bool ok = false;
            juce::String error;
        };

        const RuntimeState& runtime()
        {
            static RuntimeState state;
            static std::once_flag once;
            std::call_once (once, []
            {
               #if JUCE_WINDOWS
                // Next to the plugin binary (VST3: Contents/x86_64-win) or the standalone exe.
                const auto dir = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory();
                const auto dll = dir.getChildFile ("onnxruntime.dll");
                if (! dll.existsAsFile())
                {
                    state.error = "onnxruntime.dll is missing from " + dir.getFullPathName();
                    return;
                }
                if (LoadLibraryExW (dll.getFullPathName().toWideCharPointer(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH) == nullptr)
                {
                    state.error = "Could not load " + dll.getFullPathName() + " (error " + juce::String ((int) GetLastError()) + ")";
                    return;
                }
               #endif
                const OrtApiBase* base = OrtGetApiBase();
                const OrtApi* api = base != nullptr ? base->GetApi (ORT_API_VERSION) : nullptr;
                if (api == nullptr)
                {
                    state.error = "ONNX Runtime is older than this build needs (API " + juce::String (ORT_API_VERSION) + ")";
                    return;
                }
                Ort::InitApi (api);
                state.ok = true;
            });
            return state;
        }

        Ort::Env& sharedEnv()
        {
            static Ort::Env env (ORT_LOGGING_LEVEL_WARNING, "VocalEngine");
            return env;
        }

        class OnnxBackend final : public InferenceBackend
        {
        public:
            explicit OnnxBackend (BackendOptions o)
            {
                options.SetIntraOpNumThreads (juce::jmax (1, o.threads));
                options.SetInterOpNumThreads (1);
                options.SetGraphOptimizationLevel (GraphOptimizationLevel::ORT_ENABLE_ALL);
            }

            juce::String getName() const override { return "ONNX Runtime " + juce::String (Ort::GetVersionString()); }

            juce::Result loadModel (const juce::File& f) override
            {
                juce::MemoryBlock mb;
                if (! f.loadFileAsData (mb))
                    return juce::Result::fail ("Cannot read model " + f.getFullPathName());
                return loadModel (mb.getData(), mb.getSize());
            }

            juce::Result loadModel (const void* data, size_t size) override
            {
                try
                {
                    session = std::make_unique<Ort::Session> (sharedEnv(), data, size, options);
                    inputs = describe (true);
                    outputs = describe (false);
                    return juce::Result::ok();
                }
                catch (const Ort::Exception& e) { session.reset(); return juce::Result::fail (e.what()); }
            }

            std::vector<TensorInfo> getInputs() const override  { return inputs; }
            std::vector<TensorInfo> getOutputs() const override { return outputs; }

            juce::Result run (const std::vector<Tensor>& in, std::vector<Tensor>& out) override
            {
                if (session == nullptr)
                    return juce::Result::fail ("No model loaded");
                try
                {
                    auto mem = Ort::MemoryInfo::CreateCpu (OrtArenaAllocator, OrtMemTypeDefault);
                    std::vector<Ort::Value> values;
                    std::vector<std::string> inNames;
                    for (auto& t : in)
                    {
                        if (t.type == Tensor::Type::int64)
                            values.push_back (Ort::Value::CreateTensor<std::int64_t> (mem, const_cast<std::int64_t*> (t.ints.data()), t.ints.size(),
                                                                                    t.shape.data(), t.shape.size()));
                        else
                            values.push_back (Ort::Value::CreateTensor<float> (mem, const_cast<float*> (t.data.data()), t.data.size(),
                                                                              t.shape.data(), t.shape.size()));
                        inNames.push_back (t.name.toStdString());
                    }

                    std::vector<std::string> outNames;
                    for (auto& o : outputs) outNames.push_back (o.name.toStdString());

                    std::vector<const char*> inPtrs, outPtrs;
                    for (auto& n : inNames) inPtrs.push_back (n.c_str());
                    for (auto& n : outNames) outPtrs.push_back (n.c_str());

                    auto results = session->Run (Ort::RunOptions { nullptr }, inPtrs.data(), values.data(), values.size(),
                                                 outPtrs.data(), outPtrs.size());
                    out.clear();
                    for (size_t i = 0; i < results.size(); ++i)
                    {
                        auto info = results[i].GetTensorTypeAndShapeInfo();
                        Tensor t;
                        t.name = outNames[i];
                        t.shape = info.GetShape();
                        const auto count = info.GetElementCount();
                        if (info.GetElementType() == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64)
                        {
                            const auto* p = results[i].GetTensorData<std::int64_t>();
                            t.type = Tensor::Type::int64;
                            t.ints.assign (p, p + count);
                        }
                        else if (info.GetElementType() == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
                        {
                            const auto* p = results[i].GetTensorData<float>();
                            t.data.assign (p, p + count);
                        }
                        else
                        {
                            return juce::Result::fail ("Unsupported output type for " + t.name);
                        }
                        out.push_back (std::move (t));
                    }
                    return juce::Result::ok();
                }
                catch (const Ort::Exception& e) { return juce::Result::fail (e.what()); }
            }

        private:
            std::vector<TensorInfo> describe (bool in) const
            {
                std::vector<TensorInfo> v;
                Ort::AllocatorWithDefaultOptions alloc;
                const size_t n = in ? session->GetInputCount() : session->GetOutputCount();
                for (size_t i = 0; i < n; ++i)
                {
                    TensorInfo ti;
                    ti.name = in ? session->GetInputNameAllocated (i, alloc).get() : session->GetOutputNameAllocated (i, alloc).get();
                    auto type = in ? session->GetInputTypeInfo (i) : session->GetOutputTypeInfo (i);
                    if (type.GetONNXType() == ONNX_TYPE_TENSOR)
                        ti.rank = (int) type.GetTensorTypeAndShapeInfo().GetDimensionsCount();
                    v.push_back (ti);
                }
                return v;
            }

            Ort::SessionOptions options;
            std::unique_ptr<Ort::Session> session;
            std::vector<TensorInfo> inputs, outputs;
        };
    }

    bool isOnnxAvailable() noexcept
    {
        try { return runtime().ok; } catch (...) { return false; }
    }

    juce::String onnxUnavailableReason() { return runtime().error; }

    std::unique_ptr<InferenceBackend> createOnnxBackend (BackendOptions o)
    {
        if (! isOnnxAvailable()) return nullptr;
        return std::make_unique<OnnxBackend> (o);
    }
#else
    bool isOnnxAvailable() noexcept { return false; }
    juce::String onnxUnavailableReason() { return "This build has no ONNX Runtime (VE_WITH_ONNX=OFF)"; }
    std::unique_ptr<InferenceBackend> createOnnxBackend (BackendOptions) { return nullptr; }
#endif
}
