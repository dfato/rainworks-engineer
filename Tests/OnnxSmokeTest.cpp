// Loads a tiny in-memory ONNX model and runs one inference on a JobQueue
// worker thread. Built only with -DVE_WITH_ONNX=ON. Exit code 0 = pass.

#include "Engine/JobQueue.h"
#include "Engine/ML/InferenceBackend.h"
#include <iostream>

int main()
{

    ve::JobQueue queue (1, std::make_unique<ve::ImmediateDispatcher>());
    std::atomic<bool> passed { false };
    juce::String report;

    queue.submit ("ONNX smoke test", {}, ve::JobPriority::normal,
        [&] (ve::JobContext& ctx)
        {
            auto backend = ve::ml::createOnnxBackend();
            if (backend == nullptr)
                return juce::Result::fail ("Built without ONNX Runtime");

            const auto model = ve::ml::makeSmokeTestModel();
            if (auto r = backend->loadModel (model.getData(), model.getSize()); r.failed())
                return r;
            ctx.setProgress (0.5f);

            std::vector<ve::ml::Tensor> out;
            if (auto r = backend->run ({ { "X", { 4 }, { 1.0f, 2.0f, 3.0f, 4.5f } } }, out); r.failed())
                return r;

            if (out.size() != 1 || out[0].data.size() != 4)
                return juce::Result::fail ("Unexpected output shape");
            const float expected[] = { 2.0f, 4.0f, 6.0f, 9.0f };
            for (int i = 0; i < 4; ++i)
                if (std::abs (out[0].data[(size_t) i] - expected[i]) > 1.0e-6f)
                    return juce::Result::fail ("Wrong value at " + juce::String (i));

            report = backend->getName() + ", ran on thread " + juce::String::toHexString ((juce::pointer_sized_int) juce::Thread::getCurrentThreadId());
            return juce::Result::ok();
        },
        [&] (const ve::JobResult& r)
        {
            passed = r.success;
            if (! r.success) report = r.error;
        });

    queue.waitUntilIdle (30000);
    std::cout << (passed ? "PASS: " : "FAIL: ") << report << std::endl;
    return passed ? 0 : 1;
}
