#include "InferenceBackend.h"

#if VE_WITH_ONNX
 #include <onnxruntime_cxx_api.h>
#endif

namespace ve::ml
{
#if VE_WITH_ONNX
    namespace
    {
        class OnnxBackend final : public InferenceBackend
        {
        public:
            OnnxBackend() : env (ORT_LOGGING_LEVEL_WARNING, "VocalEngine")
            {
                options.SetIntraOpNumThreads (1);
                options.SetGraphOptimizationLevel (GraphOptimizationLevel::ORT_ENABLE_BASIC);
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
                    session = std::make_unique<Ort::Session> (env, data, size, options);
                    return juce::Result::ok();
                }
                catch (const Ort::Exception& e) { return juce::Result::fail (e.what()); }
            }

            juce::Result run (const std::vector<Tensor>& inputs, std::vector<Tensor>& outputs) override
            {
                if (session == nullptr)
                    return juce::Result::fail ("No model loaded");
                try
                {
                    auto mem = Ort::MemoryInfo::CreateCpu (OrtArenaAllocator, OrtMemTypeDefault);
                    std::vector<Ort::Value> values;
                    std::vector<std::string> inNames;
                    for (auto& t : inputs)
                    {
                        values.push_back (Ort::Value::CreateTensor<float> (mem, const_cast<float*> (t.data.data()), t.data.size(),
                                                                          t.shape.data(), t.shape.size()));
                        inNames.push_back (t.name.toStdString());
                    }

                    Ort::AllocatorWithDefaultOptions alloc;
                    std::vector<std::string> outNames;
                    for (size_t i = 0; i < session->GetOutputCount(); ++i)
                        outNames.push_back (session->GetOutputNameAllocated (i, alloc).get());

                    std::vector<const char*> inPtrs, outPtrs;
                    for (auto& n : inNames) inPtrs.push_back (n.c_str());
                    for (auto& n : outNames) outPtrs.push_back (n.c_str());

                    auto results = session->Run (Ort::RunOptions { nullptr }, inPtrs.data(), values.data(), values.size(),
                                                 outPtrs.data(), outPtrs.size());
                    outputs.clear();
                    for (size_t i = 0; i < results.size(); ++i)
                    {
                        auto info = results[i].GetTensorTypeAndShapeInfo();
                        Tensor t;
                        t.name = outNames[i];
                        t.shape = info.GetShape();
                        const auto* p = results[i].GetTensorData<float>();
                        t.data.assign (p, p + info.GetElementCount());
                        outputs.push_back (std::move (t));
                    }
                    return juce::Result::ok();
                }
                catch (const Ort::Exception& e) { return juce::Result::fail (e.what()); }
            }

        private:
            Ort::Env env;
            Ort::SessionOptions options;
            std::unique_ptr<Ort::Session> session;
        };
    }

    bool isOnnxAvailable() noexcept { return true; }
    std::unique_ptr<InferenceBackend> createOnnxBackend() { return std::make_unique<OnnxBackend>(); }
#else
    bool isOnnxAvailable() noexcept { return false; }
    std::unique_ptr<InferenceBackend> createOnnxBackend() { return nullptr; }
#endif
}
