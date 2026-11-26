#include "PitchTracker.h"
#include "ML/NeuralPitch.h"
#include "ML/TechniqueDetector.h"
#include <cmath>

namespace ve::PitchTracker
{
    namespace
    {
        // Linear interpolation of a 10 ms series at fractional frame `p`.
        float at (const std::vector<float>& v, double p) noexcept
        {
            if (v.empty()) return 0.0f;
            const int a = juce::jlimit (0, (int) v.size() - 1, (int) std::floor (p));
            const int b = juce::jmin (a + 1, (int) v.size() - 1);
            const float t = (float) juce::jlimit (0.0, 1.0, p - a);
            return v[(size_t) a] + (v[(size_t) b] - v[(size_t) a]) * t;
        }

        // f0 at fractional frame `p`: interpolated in log frequency between two voiced frames,
        // else the nearer voiced one.
        float f0At (const std::vector<float>& v, double p) noexcept
        {
            if (v.empty()) return 0.0f;
            const int a = juce::jlimit (0, (int) v.size() - 1, (int) std::floor (p));
            const int b = juce::jmin (a + 1, (int) v.size() - 1);
            const float fa = v[(size_t) a], fb = v[(size_t) b];
            const float t = (float) juce::jlimit (0.0, 1.0, p - a);
            if (fa > 0.0f && fb > 0.0f) return fa * std::pow (fb / fa, t);
            return t < 0.5f ? fa : fb;
        }
    }

    juce::Result trackBest (const juce::AudioBuffer<float>& audio, double sr, const Settings& s,
                            JobContext* ctx, PitchTrack& out, Engine engine, const NeuralSettings& ns,
                            juce::String* status)
    {
        // YIN always runs: it is cheap and gives the loudness of each frame, and the track
        // keeps its hop size, so everything downstream (notes, intent, PSOLA) is unchanged.
        if (auto r = track (audio, sr, s, ctx, out); r.failed()) return r;
        out.engine = PitchEngine::dsp;
        if (engine == Engine::dsp)
        {
            if (status != nullptr) *status = "DSP pitch tracker";
            return juce::Result::ok();
        }

        juce::String why;
        if (! NeuralPitch::isAvailable (&why))
        {
            if (engine == Engine::neural) return juce::Result::fail ("Neural pitch unavailable: " + why);
            if (status != nullptr) *status = "DSP pitch tracker (" + why + ")";
            return juce::Result::ok();
        }

        if (ctx != nullptr && ctx->shouldCancel()) return juce::Result::fail ("cancelled");
        const auto mono16 = ml::resampleToMono (audio, sr, ml::MelSpectrogram::sampleRate);
        const auto mel = ml::MelSpectrogram::compute (mono16);
        NeuralPitch::Frames nf;
        if (auto r = NeuralPitch::run (mel, nullptr, nf, 0.0f); r.failed())
        {
            if (engine == Engine::neural || r.getErrorMessage() == "cancelled") return r;
            if (status != nullptr) *status = "DSP pitch tracker (" + r.getErrorMessage() + ")";
            return juce::Result::ok();
        }

        const float silence = juce::Decibels::decibelsToGain (s.silenceDb);
        const double framesPerSample = (double) ml::MelSpectrogram::sampleRate / ml::MelSpectrogram::hopSize / sr;
        for (int i = 0; i < (int) out.frames.size(); ++i)
        {
            auto& f = out.frames[(size_t) i];
            const double p = out.frameToSample (i) * framesPerSample;
            const float conf = at (nf.confidence, p);
            float f0 = f0At (nf.f0, p);
            const float yin = f.f0, yinConf = f.confidence;
            const float apart = (yin > 0.0f && f0 > 0.0f) ? std::abs (1200.0f * std::log2 (yin / f0)) : 1.0e9f;
            const bool agree = apart <= 50.0f;
            bool voiced = conf >= ns.voicingThreshold;
            // Weak network peak but a clean YIN reading at the same pitch: a quiet or very
            // steady tone the network is unsure of, so keep it.
            if (! voiced && ns.rescueThreshold > 0.0f && conf >= ns.rescueThreshold && agree && yinConf >= 0.8f)
                voiced = true;
            if (apart <= ns.refineCents)
                f0 = yin;   // YIN is finer on a clean period; the network has already ruled out octave slips
            f.confidence = conf;
            f.f0 = (voiced && f.rms >= silence && f0 >= s.minHz * 0.5f && f0 <= s.maxHz * 1.5f) ? f0 : 0.0f;
        }
        out.engine = PitchEngine::neural;

        juce::String techStatus;
        if (ns.techniques)
        {
            std::vector<TechniqueFrame> tf;
            juce::String twhy;
            if (TechniqueDetector::isAvailable (&twhy) && TechniqueDetector::run (mel, nullptr, tf).wasOk() && ! tf.empty())
            {
                out.technique.resize (out.frames.size());
                for (int i = 0; i < (int) out.frames.size(); ++i)
                {
                    const double p = out.frameToSample (i) * framesPerSample;
                    const int a = juce::jlimit (0, (int) tf.size() - 1, (int) std::floor (p));
                    const int b = juce::jmin (a + 1, (int) tf.size() - 1);
                    const float t = (float) juce::jlimit (0.0, 1.0, p - a);
                    for (int k = 0; k < TechniqueFrame::count; ++k)
                        out.technique[(size_t) i].p[k] = tf[(size_t) a].p[k] + (tf[(size_t) b].p[k] - tf[(size_t) a].p[k]) * t;
                }
                techStatus = " + technique detection";
            }
            else
                techStatus = " (technique detection off: " + (twhy.isNotEmpty() ? twhy : juce::String ("failed")) + ")";
        }
        if (status != nullptr) *status = "Neural pitch (FCPE)" + techStatus;
        if (ctx != nullptr) ctx->setProgress (1.0f);
        return juce::Result::ok();
    }
}
