#include "NeuralPitch.h"
#include "SharedModel.h"
#include "../Voice/VoiceModel.h"
#include <cmath>

namespace ve::NeuralPitch
{
    namespace
    {
        // FCPE's cent table: linspace(cents(32.7), cents(1975.5), 360), cents(f) = 1200 log2(f / 10).
        const std::vector<float>& centTable()
        {
            static const std::vector<float> t = []
            {
                std::vector<float> v ((size_t) numBins);
                const double c0 = 1200.0 * std::log2 (minHz / 10.0), c1 = 1200.0 * std::log2 (maxHz / 10.0);
                for (int i = 0; i < numBins; ++i) v[(size_t) i] = (float) (c0 + (c1 - c0) * i / (numBins - 1));
                return v;
            }();
            return t;
        }

        ml::SharedModel& model()
        {
            static ml::SharedModel m ([] (ml::InferenceBackend& b) -> juce::String
            {
                const auto f = findModel();
                if (f == juce::File()) return juce::String ("the pitch model (") + modelFileName + ") isn't installed";
                if (auto r = b.loadModel (f); r.failed()) return "could not load " + f.getFullPathName() + ": " + r.getErrorMessage();
                if (b.getInputs().size() != 1 || b.getOutputs().empty()) return f.getFileName() + " is not an FCPE pitch model";
                return {};
            }, ml::BackendOptions { 2, false });
            return m;
        }
    }

    juce::File findModel()
    {
        if (auto env = juce::SystemStats::getEnvironmentVariable ("VE_PITCH_MODEL", {}); env.isNotEmpty())
        {
            const juce::File f (env);
            if (f.existsAsFile()) return f;
        }
        for (auto loc : { juce::File::currentExecutableFile, juce::File::currentApplicationFile })
        {
            const auto f = juce::File::getSpecialLocation (loc).getParentDirectory().getChildFile (modelFileName);
            if (f.existsAsFile()) return f;
        }
        const auto f = VoiceLibrary::modelsFolder().getChildFile (modelFileName);
        return f.existsAsFile() ? f : juce::File();
    }

    bool isAvailable (juce::String* why) { return model().isLoaded (why); }

    juce::String describe()
    {
        juce::String why;
        if (! isAvailable (&why)) return "Pitch: DSP tracker (neural pitch off: " + why + ")";
        return "Pitch: neural (FCPE) on " + model().getDevice();
    }

    float binToHz (float bin) noexcept
    {
        const auto& t = centTable();
        const float c = t[0] + (t[(size_t) numBins - 1] - t[0]) * bin / (float) (numBins - 1);
        return 10.0f * std::pow (2.0f, c / 1200.0f);
    }

    void decode (const float* y, float& f0, float& confidence, float threshold) noexcept
    {
        int best = 0;
        for (int i = 1; i < numBins; ++i) if (y[i] > y[best]) best = i;
        confidence = y[best];
        const auto& t = centTable();
        double num = 0.0, den = 0.0;
        for (int i = juce::jmax (0, best - 4); i <= juce::jmin (numBins - 1, best + 4); ++i)
        {
            num += (double) t[(size_t) i] * y[i];
            den += y[i];
        }
        f0 = (confidence > threshold && den > 0.0) ? 10.0f * (float) std::pow (2.0, num / den / 1200.0) : 0.0f;
        if (f0 < minHz) f0 = 0.0f;
    }

    juce::Result run (const ml::MelSpectrogram& mel, JobContext* ctx, Frames& out, float threshold)
    {
        out = {};
        if (mel.numFrames <= 0) return juce::Result::ok();
        juce::String why;
        if (! isAvailable (&why)) return juce::Result::fail (why);

        const auto inName = model().getInputs().front().name;
        out.f0.assign ((size_t) mel.numFrames, 0.0f);
        out.confidence.assign ((size_t) mel.numFrames, 0.0f);

        // Chunks of 30 s with 1.5 s of context either side: the network sees about ±1 s, so
        // the joins are exact, and memory stays flat on whole songs.
        constexpr int chunk = 3000, context = 150;
        for (int s = 0; s < mel.numFrames; s += chunk)
        {
            if (ctx != nullptr)
            {
                if (ctx->shouldCancel()) return juce::Result::fail ("cancelled");
                ctx->setProgress ((float) s / (float) mel.numFrames);
            }
            const int e = juce::jmin (mel.numFrames, s + chunk);
            const int a = juce::jmax (0, s - context), b = juce::jmin (mel.numFrames, e + context);
            std::vector<float> x (mel.data.begin() + (std::ptrdiff_t) a * ml::MelSpectrogram::numMels,
                                  mel.data.begin() + (std::ptrdiff_t) b * ml::MelSpectrogram::numMels);
            std::vector<ml::Tensor> res;
            if (auto r = model().run ({ ml::Tensor::makeFloat (inName, { 1, b - a, ml::MelSpectrogram::numMels }, std::move (x)) }, res); r.failed())
                return juce::Result::fail ("Neural pitch: " + r.getErrorMessage());
            if (res.empty() || res[0].shape.size() != 3 || res[0].shape[1] != b - a || res[0].shape[2] != numBins)
                return juce::Result::fail ("Neural pitch: unexpected output shape");
            for (int i = s; i < e; ++i)
                decode (res[0].data.data() + (size_t) (i - a) * numBins, out.f0[(size_t) i], out.confidence[(size_t) i], threshold);
        }
        if (ctx != nullptr) ctx->setProgress (1.0f);
        return juce::Result::ok();
    }
}
