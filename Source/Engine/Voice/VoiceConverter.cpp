#include "VoiceConverter.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <algorithm>
#include <cmath>
#include <random>

namespace ve
{
    namespace
    {
        std::vector<float> resampleMono (const float* in, int n, double from, double to)
        {
            if (n <= 0) return {};
            if (std::abs (from - to) < 0.5) return { in, in + n };
            const double ratio = from / to;
            const int outLen = (int) std::llround ((double) n / ratio);
            std::vector<float> out ((size_t) outLen, 0.0f);
            juce::LagrangeInterpolator interp;
            interp.process (ratio, in, out.data(), outLen, n, 0);
            return out;
        }

        bool hasInput (const ml::InferenceBackend& b, const char* name)
        {
            for (auto& i : b.getInputs())
                if (i.name == name) return true;
            return false;
        }
    }

    VoiceConverter::VoiceConverter (ml::InferenceBackend& e, ml::InferenceBackend& s, const VoiceModelInfo& m, const FeatureBank* fb)
        : encoder (e), synth (s), model (m), bank (fb != nullptr && fb->size() > 0 && fb->getDim() == m.featureDim ? fb : nullptr)
    {
    }

    juce::String VoiceConverter::validate() const
    {
        if (encoder.getInputs().empty() || encoder.getOutputs().empty())
            return "content encoder has no inputs/outputs";
        for (auto* name : { "phone", "phone_lengths", "pitch", "pitchf", "ds", "rnd" })
            if (! hasInput (synth, name))
                return juce::String ("voice model is missing input \"") + name + "\" (only pitch-aware RVC exports are supported)";
        if (synth.getOutputs().empty())
            return "voice model has no output";
        return {};
    }

    std::int64_t VoiceConverter::coarsePitch (float hz) noexcept
    {
        static const double melMin = 1127.0 * std::log (1.0 + 50.0 / 700.0);
        static const double melMax = 1127.0 * std::log (1.0 + 1100.0 / 700.0);
        if (hz <= 0.0f) return 1;
        double mel = 1127.0 * std::log (1.0 + hz / 700.0);
        mel = (mel - melMin) * 254.0 / (melMax - melMin) + 1.0;
        return (std::int64_t) std::llround (juce::jlimit (1.0, 255.0, mel));
    }

    juce::Result VoiceConverter::runEncoder (const std::vector<float>& audio, std::vector<float>& feats, int& frames)
    {
        const auto inInfo = encoder.getInputs().front();
        auto t = ml::Tensor::makeFloat (inInfo.name,
                                        inInfo.rank == 2 ? std::vector<std::int64_t> { 1, (std::int64_t) audio.size() }
                                                         : std::vector<std::int64_t> { 1, 1, (std::int64_t) audio.size() },
                                        audio);
        std::vector<ml::Tensor> out;
        if (auto r = encoder.run ({ t }, out); r.failed())
            return juce::Result::fail ("Content encoder: " + r.getErrorMessage());
        if (out.empty() || out[0].shape.size() != 3)
            return juce::Result::fail ("Content encoder: unexpected output shape");

        const auto& o = out[0];
        const int D = model.featureDim;
        if (o.shape[2] == D)            // [1, frames, D]
        {
            frames = (int) o.shape[1];
            feats = o.data;
        }
        else if (o.shape[1] == D)       // [1, D, frames]
        {
            frames = (int) o.shape[2];
            feats.assign ((size_t) frames * (size_t) D, 0.0f);
            for (int d = 0; d < D; ++d)
                for (int f = 0; f < frames; ++f)
                    feats[(size_t) f * (size_t) D + (size_t) d] = o.data[(size_t) d * (size_t) frames + (size_t) f];
        }
        else
        {
            return juce::Result::fail ("Content encoder gives " + juce::String (o.shape[1]) + "x" + juce::String (o.shape[2])
                                       + " features; the voice model expects " + juce::String (D));
        }
        return juce::Result::ok();
    }

    juce::Result VoiceConverter::convert (const float* in, int n, double inRate, const std::vector<float>& f0,
                                          const Settings& st, JobContext* ctx, std::vector<float>& out)
    {
        out.clear();
        if (n <= 0 || inRate <= 0.0) return juce::Result::ok();
        if (auto err = validate(); err.isNotEmpty()) return juce::Result::fail (err);

        const auto audio16 = resampleMono (in, n, inRate, encoderRate);
        const int hop16 = (int) (encoderRate / frameRate);                  // 160
        const int totalFrames = (int) ((audio16.size() + (size_t) hop16 - 1) / (size_t) hop16);
        const int hopOut = model.hopSize();
        const int D = model.featureDim;

        // Planned pitch per synthesiser frame.
        std::vector<float> framePitch ((size_t) totalFrames, 0.0f);
        for (int t = 0; t < totalFrames; ++t)
        {
            const int i = juce::jlimit (0, n - 1, (int) std::llround (((double) t + 0.5) * inRate / frameRate));
            framePitch[(size_t) t] = i < (int) f0.size() ? f0[(size_t) i] : 0.0f;
        }

        std::vector<float> result ((size_t) totalFrames * (size_t) hopOut, 0.0f);
        std::vector<float> weight (result.size(), 0.0f);

        const int chunk = juce::jmax (50, (int) (st.chunkSeconds * frameRate));
        const int overlap = juce::jlimit (0, chunk / 2, (int) (st.overlapSeconds * frameRate));
        const int context = juce::jmax (0, (int) (st.contextSeconds * frameRate));
        std::mt19937 rng (st.seed);
        std::normal_distribution<float> gauss (0.0f, 1.0f);

        for (int a = 0; a < totalFrames; a += chunk - overlap)
        {
            if (ctx != nullptr && ctx->shouldCancel()) return juce::Result::fail ("cancelled");
            const int b = juce::jmin (totalFrames, a + chunk);
            const int pa = juce::jmax (0, a - context), pb = juce::jmin (totalFrames, b + context);
            const int T = pb - pa;

            // Encoder on the padded chunk (plus one encoder window so the frame count covers it).
            const size_t s0 = (size_t) pa * (size_t) hop16;
            const size_t s1 = juce::jmin (audio16.size(), (size_t) pb * (size_t) hop16 + 400);
            std::vector<float> seg (audio16.begin() + (std::ptrdiff_t) s0, audio16.begin() + (std::ptrdiff_t) s1);
            seg.resize (juce::jmax (seg.size(), (size_t) T * (size_t) hop16 + 400), 0.0f);
            std::vector<float> feats;
            int F = 0;
            if (auto r = runEncoder (seg, feats, F); r.failed()) return r;
            if (F <= 0) return juce::Result::fail ("Content encoder returned no frames");

            // Retrieval + protect at 50 fps, then repeat each frame twice for 100 fps.
            std::vector<float> phone ((size_t) T * (size_t) D, 0.0f);
            std::vector<float> retrieved ((size_t) D);
            for (int f = 0; f < F && 2 * f < T; ++f)
            {
                const float* src = feats.data() + (size_t) f * (size_t) D;
                const bool voiced = framePitch[(size_t) (pa + 2 * f)] > 0.0f;
                if (bank != nullptr && st.indexRate > 0.0f)
                    bank->retrieve (src, retrieved.data());
                for (int d = 0; d < D; ++d)
                {
                    float v = src[d];
                    if (bank != nullptr && st.indexRate > 0.0f)
                    {
                        const float mixed = st.indexRate * retrieved[(size_t) d] + (1.0f - st.indexRate) * v;
                        // protect: on unvoiced frames lean back toward the original features.
                        v = (! voiced && st.protect < 0.5f) ? st.protect * mixed + (1.0f - st.protect) * v : mixed;
                    }
                    for (int rep = 0; rep < 2 && 2 * f + rep < T; ++rep)
                        phone[(size_t) (2 * f + rep) * (size_t) D + (size_t) d] = v;
                }
            }
            // Encoder gives slightly fewer frames than T: hold the last one.
            const int filled = juce::jmin (T, 2 * F);
            for (int t = filled; t < T; ++t)
                std::copy_n (phone.begin() + (std::ptrdiff_t) ((size_t) (filled - 1) * (size_t) D), D,
                             phone.begin() + (std::ptrdiff_t) ((size_t) t * (size_t) D));

            std::vector<std::int64_t> pitch ((size_t) T);
            std::vector<float> pitchf ((size_t) T);
            for (int t = 0; t < T; ++t)
            {
                pitchf[(size_t) t] = framePitch[(size_t) (pa + t)];
                pitch[(size_t) t] = coarsePitch (pitchf[(size_t) t]);
            }
            std::vector<float> rnd ((size_t) 192 * (size_t) T);
            for (auto& x : rnd) x = gauss (rng);

            std::vector<ml::Tensor> inputs;
            inputs.push_back (ml::Tensor::makeFloat ("phone", { 1, T, D }, std::move (phone)));
            inputs.push_back (ml::Tensor::makeInt64 ("phone_lengths", { 1 }, { T }));
            inputs.push_back (ml::Tensor::makeInt64 ("pitch", { 1, T }, std::move (pitch)));
            inputs.push_back (ml::Tensor::makeFloat ("pitchf", { 1, T }, std::move (pitchf)));
            inputs.push_back (ml::Tensor::makeInt64 ("ds", { 1 }, { model.speakerId }));
            inputs.push_back (ml::Tensor::makeFloat ("rnd", { 1, 192, T }, std::move (rnd)));
            std::vector<ml::Tensor> outputs;
            if (auto r = synth.run (inputs, outputs); r.failed())
                return juce::Result::fail ("Voice model: " + r.getErrorMessage());
            if (outputs.empty() || outputs[0].data.empty())
                return juce::Result::fail ("Voice model returned no audio");
            const auto& wav = outputs[0].data;

            // Keep [a, b) of the padded render, with linear fades over the overlaps.
            const int keepFrom = (a - pa) * hopOut, keepTo = (b - pa) * hopOut;
            const int fadeIn = a > 0 ? overlap * hopOut : 0;
            const int fadeOut = b < totalFrames ? overlap * hopOut : 0;
            for (int i = keepFrom; i < keepTo && i < (int) wav.size(); ++i)
            {
                const int local = i - keepFrom, fromEnd = keepTo - 1 - i;
                float w = 1.0f;
                if (fadeIn > 0 && local < fadeIn) w = juce::jmin (w, (float) (local + 1) / (float) (fadeIn + 1));
                if (fadeOut > 0 && fromEnd < fadeOut) w = juce::jmin (w, (float) (fromEnd + 1) / (float) (fadeOut + 1));
                const size_t dst = (size_t) a * (size_t) hopOut + (size_t) local;
                if (dst >= result.size()) break;
                result[dst] += w * wav[(size_t) i];
                weight[dst] += w;
            }

            if (ctx != nullptr) ctx->setProgress (0.3f + 0.6f * (float) b / (float) totalFrames);
            if (b >= totalFrames) break;
        }

        for (size_t i = 0; i < result.size(); ++i)
            if (weight[i] > 1.0e-6f) result[i] /= weight[i];

        const auto wanted = (size_t) std::llround ((double) n * model.sampleRate / inRate);
        result.resize (wanted, 0.0f);
        out = std::move (result);
        return juce::Result::ok();
    }
}
