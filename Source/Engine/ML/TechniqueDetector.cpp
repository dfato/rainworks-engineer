#include "TechniqueDetector.h"
#include "SharedModel.h"
#include "VEModelData.h"

namespace ve::TechniqueDetector
{
    namespace
    {
        ml::SharedModel& model()
        {
            static ml::SharedModel m ([] (ml::InferenceBackend& b) -> juce::String
            {
                if (auto r = b.loadModel (VEModels::techniqueclassifier_onnx, (size_t) VEModels::techniqueclassifier_onnxSize); r.failed())
                    return "could not load the technique model: " + r.getErrorMessage();
                if (b.getInputs().size() != 1 || b.getOutputs().size() != 1) return "the technique model has unexpected inputs";
                return {};
            }, ml::BackendOptions { 1, false });
            return m;
        }
    }

    bool isAvailable (juce::String* why) { return model().isLoaded (why); }

    const char* kindName (int k) noexcept
    {
        switch (k)
        {
            case TechniqueFrame::vibrato: return "vibrato";
            case TechniqueFrame::breathy: return "breathy";
            case TechniqueFrame::belt:    return "belt";
            case TechniqueFrame::grit:    return "grit";
            case TechniqueFrame::spoken:  return "spoken";
            default: return "";
        }
    }

    juce::Result run (const ml::MelSpectrogram& mel, JobContext* ctx, std::vector<TechniqueFrame>& out)
    {
        out.clear();
        if (mel.numFrames <= 0) return juce::Result::ok();
        juce::String why;
        if (! isAvailable (&why)) return juce::Result::fail (why);
        const auto inName = model().getInputs().front().name;
        out.resize ((size_t) mel.numFrames);

        // The network sees about ±0.6 s; chunks of 20 s with 1 s of context either side.
        constexpr int chunk = 2000, context = 100;
        for (int s = 0; s < mel.numFrames; s += chunk)
        {
            if (ctx != nullptr && ctx->shouldCancel()) return juce::Result::fail ("cancelled");
            const int e = juce::jmin (mel.numFrames, s + chunk);
            const int a = juce::jmax (0, s - context), b = juce::jmin (mel.numFrames, e + context);
            std::vector<float> x (mel.data.begin() + (std::ptrdiff_t) a * ml::MelSpectrogram::numMels,
                                  mel.data.begin() + (std::ptrdiff_t) b * ml::MelSpectrogram::numMels);
            std::vector<ml::Tensor> res;
            if (auto r = model().run ({ ml::Tensor::makeFloat (inName, { 1, b - a, ml::MelSpectrogram::numMels }, std::move (x)) }, res); r.failed())
                return juce::Result::fail ("Technique model: " + r.getErrorMessage());
            if (res.empty() || res[0].shape.size() != 3 || res[0].shape[1] != b - a || res[0].shape[2] != TechniqueFrame::count)
                return juce::Result::fail ("Technique model: unexpected output shape");
            for (int i = s; i < e; ++i)
                for (int k = 0; k < TechniqueFrame::count; ++k)
                    out[(size_t) i].p[k] = res[0].data[(size_t) ((i - a) * TechniqueFrame::count + k)];
        }
        return juce::Result::ok();
    }

    TechniqueFrame summarise (const PitchTrack& t, std::int64_t start, std::int64_t end)
    {
        TechniqueFrame m;
        if (! t.hasTechnique() || t.hopSize <= 0) return m;
        const int a = juce::jlimit (0, (int) t.frames.size(), (int) (start / t.hopSize));
        const int b = juce::jlimit (a, (int) t.frames.size(), (int) ((end + t.hopSize - 1) / t.hopSize));
        const float floor = juce::Decibels::decibelsToGain (-50.0f);
        int count = 0;
        for (int i = a; i < b; ++i)
        {
            if (t.frames[(size_t) i].rms < floor) continue;
            for (int k = 0; k < TechniqueFrame::count; ++k) m.p[k] += t.technique[(size_t) i].p[k];
            ++count;
        }
        if (count > 0)
            for (auto& v : m.p) v /= (float) count;
        return m;
    }
}
