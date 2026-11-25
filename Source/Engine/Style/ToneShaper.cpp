#include "ToneShaper.h"
#include <cmath>
#include <random>

namespace ve::ToneShaper
{
    namespace
    {
        // RBJ cookbook biquad, transposed direct form II.
        struct Biquad
        {
            double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;

            static Biquad shelf (double sr, double freq, double gainDb, bool high)
            {
                const double A = std::pow (10.0, gainDb / 40.0), w = juce::MathConstants<double>::twoPi * freq / sr;
                const double cs = std::cos (w), al = std::sin (w) / 2.0 * std::sqrt (2.0), sq = 2.0 * std::sqrt (A) * al;
                Biquad q; double a0;
                if (high)
                {
                    q.b0 = A * ((A + 1) + (A - 1) * cs + sq); q.b1 = -2 * A * ((A - 1) + (A + 1) * cs); q.b2 = A * ((A + 1) + (A - 1) * cs - sq);
                    a0 = (A + 1) - (A - 1) * cs + sq; q.a1 = 2 * ((A - 1) - (A + 1) * cs); q.a2 = (A + 1) - (A - 1) * cs - sq;
                }
                else
                {
                    q.b0 = A * ((A + 1) - (A - 1) * cs + sq); q.b1 = 2 * A * ((A - 1) - (A + 1) * cs); q.b2 = A * ((A + 1) - (A - 1) * cs - sq);
                    a0 = (A + 1) + (A - 1) * cs + sq; q.a1 = -2 * ((A - 1) + (A + 1) * cs); q.a2 = (A + 1) + (A - 1) * cs - sq;
                }
                q.norm (a0);
                return q;
            }
            static Biquad peak (double sr, double freq, double gainDb, double Q)
            {
                const double A = std::pow (10.0, gainDb / 40.0), w = juce::MathConstants<double>::twoPi * freq / sr;
                const double al = std::sin (w) / (2.0 * Q), cs = std::cos (w);
                Biquad q;
                q.b0 = 1 + al * A; q.b1 = -2 * cs; q.b2 = 1 - al * A;
                q.a1 = -2 * cs; q.a2 = 1 - al / A;
                q.norm (1 + al / A);
                return q;
            }
            static Biquad pass (double sr, double freq, bool high)
            {
                const double w = juce::MathConstants<double>::twoPi * freq / sr, cs = std::cos (w), al = std::sin (w) / (2.0 * 0.7071);
                Biquad q;
                if (high) { q.b0 = (1 + cs) / 2; q.b1 = -(1 + cs); q.b2 = (1 + cs) / 2; }
                else      { q.b0 = (1 - cs) / 2; q.b1 = 1 - cs;    q.b2 = (1 - cs) / 2; }
                q.a1 = -2 * cs; q.a2 = 1 - al;
                q.norm (1 + al);
                return q;
            }
            void norm (double a0) { b0 /= a0; b1 /= a0; b2 /= a0; a1 /= a0; a2 /= a0; }
            float operator() (float x)
            {
                const double y = b0 * x + z1;
                z1 = b1 * x - a1 * y + z2;
                z2 = b2 * x - a2 * y;
                return (float) y;
            }
        };

        double rms (const juce::AudioBuffer<float>& b)
        {
            double s = 0.0; std::int64_t count = 0;
            for (int ch = 0; ch < b.getNumChannels(); ++ch)
                for (int i = 0; i < b.getNumSamples(); ++i) { const double x = b.getSample (ch, i); s += x * x; ++count; }
            return count > 0 ? std::sqrt (s / (double) count) : 0.0;
        }
    }

    void process (juce::AudioBuffer<float>& audio, double sr, const StyleProfile& st)
    {
        const int n = audio.getNumSamples();
        if (n == 0 || sr <= 0.0 || ! st.hasTone()) return;
        const double before = rms (audio);
        const float peakBefore = audio.getMagnitude (0, n);
        if (before < 1.0e-6) return;

        const auto envCoef = [sr] (double ms) { return (float) std::exp (-1.0 / (ms * 0.001 * sr)); };
        std::mt19937 rng (12345u);
        std::uniform_real_distribution<float> uni (-1.0f, 1.0f);

        for (int ch = 0; ch < audio.getNumChannels(); ++ch)
        {
            float* x = audio.getWritePointer (ch);

            // Voice level (fast envelope) for grit and breath.
            std::vector<float> env ((size_t) n);
            {
                const float att = envCoef (3.0), rel = envCoef (60.0);
                float e = 0.0f;
                for (int i = 0; i < n; ++i)
                {
                    const float a = std::abs (x[i]);
                    e = a > e ? a + att * (e - a) : a + rel * (e - a);
                    env[(size_t) i] = e;
                }
            }

            // Grit: saturate a band-passed copy at a level that follows the voice, and blend
            // in only the distortion it adds.
            if (st.grit > 0.0f)
            {
                auto hp = Biquad::pass (sr, 350.0, true), lp = Biquad::pass (sr, 4500.0, false);
                auto post = Biquad::pass (sr, 7000.0, false);
                const float drive = 1.5f + 6.0f * st.grit, norm = std::tanh (drive);
                for (int i = 0; i < n; ++i)
                {
                    const float band = lp (hp (x[i]));
                    const float level = juce::jmax (env[(size_t) i], 1.0e-4f);
                    const float sat = level * std::tanh (drive * band / level) / norm;
                    x[i] += 0.8f * st.grit * post (sat - band);
                }
            }

            // Breath: shaped noise riding the voice envelope.
            if (st.breath > 0.0f)
            {
                auto hp1 = Biquad::pass (sr, 1800.0, true), hp2 = Biquad::pass (sr, 1800.0, true);
                auto lp = Biquad::pass (sr, juce::jmin (11000.0, sr * 0.45), false);
                auto bump = Biquad::peak (sr, 5000.0, 4.0, 0.8);
                const float slow = envCoef (25.0);
                float e = 0.0f;
                for (int i = 0; i < n; ++i)
                {
                    e = env[(size_t) i] + slow * (e - env[(size_t) i]);
                    x[i] += st.breath * 0.22f * e * bump (lp (hp2 (hp1 (uni (rng)))));
                }
            }

            // Tone shelves.
            if (std::abs (st.warmth) > 1.0e-3f || std::abs (st.brightness) > 1.0e-3f)
            {
                auto low = Biquad::shelf (sr, 220.0, 5.0 * st.warmth, false);
                auto mud = Biquad::peak (sr, 450.0, 2.0 * st.warmth, 1.0);
                auto presence = Biquad::peak (sr, 3000.0, 4.0 * st.brightness, 0.9);
                auto air = Biquad::shelf (sr, 8000.0, 6.0 * st.brightness, true);
                for (int i = 0; i < n; ++i)
                    x[i] = air (presence (mud (low (x[i]))));
            }

            // Compression: feed-forward, threshold relative to the take's peak.
            if (st.compress > 0.0f)
            {
                const float thresh = peakBefore * std::pow (10.0f, -(10.0f + 12.0f * st.compress) / 20.0f);
                const float ratio = 1.0f + 5.0f * st.compress;
                const float att = envCoef (4.0), rel = envCoef (90.0);
                float e = 0.0f, g = 1.0f;
                for (int i = 0; i < n; ++i)
                {
                    const float a = std::abs (x[i]);
                    e = a > e ? a + att * (e - a) : a + rel * (e - a);
                    const float target = e > thresh ? std::pow (thresh / e, 1.0f - 1.0f / ratio) : 1.0f;
                    g = target + (target < g ? att : rel) * (g - target);
                    x[i] *= g;
                }
            }
        }

        // Same loudness as before; the odd peak that would then clip is rounded off softly.
        const double after = rms (audio);
        if (after > 1.0e-9)
        {
            audio.applyGain ((float) (before / after));
            const float knee = 0.9f, room = 0.09f;
            for (int ch = 0; ch < audio.getNumChannels(); ++ch)
            {
                float* x = audio.getWritePointer (ch);
                for (int i = 0; i < n; ++i)
                    if (const float a = std::abs (x[i]); a > knee)
                        x[i] = std::copysign (knee + room * std::tanh ((a - knee) / room), x[i]);
            }
        }
    }
}
