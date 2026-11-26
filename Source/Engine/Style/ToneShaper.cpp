#include "ToneShaper.h"
#include <cmath>
#include <algorithm>
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

        // How much voice there is at each sample, 0..1: 0 within 6 dB of the take's noise
        // floor, 1 from 18 dB above it. The floor is the quiet end (5th percentile) of the
        // take's 20 ms levels, ignoring digital silence, kept 45 to 80 dB under the peak.
        std::vector<float> presenceGate (const juce::AudioBuffer<float>& b, double sr)
        {
            const int n = b.getNumSamples(), chs = b.getNumChannels();
            std::vector<float> mono ((size_t) n, 0.0f);
            for (int ch = 0; ch < chs; ++ch)
                for (int i = 0; i < n; ++i) mono[(size_t) i] += b.getSample (ch, i) / (float) chs;

            const int frame = juce::jmax (1, (int) (0.02 * sr));
            std::vector<float> levels;
            float peak = 0.0f;
            for (int a = 0; a < n; a += frame)
            {
                double s = 0.0;
                const int e = juce::jmin (n, a + frame);
                for (int i = a; i < e; ++i) { s += (double) mono[(size_t) i] * mono[(size_t) i]; peak = juce::jmax (peak, std::abs (mono[(size_t) i])); }
                const float r = (float) std::sqrt (s / (double) (e - a));
                if (r > 1.0e-7f) levels.push_back (r);
            }
            float floorLevel = peak * 1.0e-4f;
            if (! levels.empty())
            {
                const auto k = levels.size() / 20;
                std::nth_element (levels.begin(), levels.begin() + (std::ptrdiff_t) k, levels.end());
                // A take with no real gaps (a dense, bled-into stem) has no noise floor to find:
                // its quiet end is singing, so the estimate is capped 45 dB under the peak.
                floorLevel = juce::jlimit (floorLevel, peak * 0.0056f, levels[k]);
            }
            const float lo = floorLevel * 2.0f, hi = floorLevel * 8.0f;   // +6 dB .. +18 dB

            // Level: RMS of 5 ms blocks, held for 40 ms after and 10 ms before (so the gate
            // closes promptly in a gap but not between the syllables of a word), mapped through
            // a smooth ramp in dB, then a 10 ms smoother both ways so the blend never clicks.
            // (A one-pole release would need most of a second to fall 60 dB to the floor.)
            const int blk = juce::jmax (1, (int) (0.005 * sr));
            const int nb = (n + blk - 1) / blk;
            std::vector<float> br ((size_t) nb, 0.0f), bt ((size_t) nb, 0.0f);
            for (int k = 0; k < nb; ++k)
            {
                double s = 0.0;
                const int a0 = k * blk, e = juce::jmin (n, a0 + blk);
                for (int i = a0; i < e; ++i) s += (double) mono[(size_t) i] * mono[(size_t) i];
                br[(size_t) k] = (float) std::sqrt (s / (double) (e - a0));
            }
            const float span = std::log (hi / lo);
            for (int k = 0; k < nb; ++k)
            {
                float lvl = 0.0f;
                for (int j = juce::jmax (0, k - 8); j <= juce::jmin (nb - 1, k + 2); ++j) lvl = juce::jmax (lvl, br[(size_t) j]);
                float t = lvl <= lo ? 0.0f : lvl >= hi ? 1.0f : std::log (lvl / lo) / span;
                bt[(size_t) k] = t * t * (3.0f - 2.0f * t);
            }
            std::vector<float> g ((size_t) n, 0.0f);
            const float sm = (float) std::exp (-1.0 / (0.01 * sr));
            float out = 0.0f;
            for (int i = 0; i < n; ++i)
            {
                const float t = bt[(size_t) (i / blk)];
                out = t + sm * (out - t);
                g[(size_t) i] = out;
            }
            // Run the smoother backwards too, so the gate opens a little before each word.
            for (int i = n - 2; i >= 0; --i) g[(size_t) i] = juce::jmax (g[(size_t) i], g[(size_t) i + 1] * sm);
            return g;
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
        std::uniform_real_distribution<float> uni (-1.0f, 1.0f);
        const juce::AudioBuffer<float> dry (audio);
        const auto gate = presenceGate (dry, sr);

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
                    x[i] += 0.8f * st.grit * gate[(size_t) i] * post (sat - band);
                }
            }

            // Breath: aspiration noise. Real breathiness is noise pulsed by every glottal
            // cycle, so the noise here is gated by the voice's own pulses (a ~0.5 ms follower
            // of the mids over a 25 ms one). Plain steady noise riding the level, as this used
            // to be, does not fuse with the voice: it is heard as a hiss bed under every note,
            // and compressors after the plugin bring it right up.
            if (st.breath > 0.0f)
            {
                auto hp1 = Biquad::pass (sr, 1800.0, true), hp2 = Biquad::pass (sr, 1800.0, true);
                auto lp = Biquad::pass (sr, juce::jmin (11000.0, sr * 0.45), false);
                auto bump = Biquad::peak (sr, 5000.0, 4.0, 0.8);
                auto mhp = Biquad::pass (sr, 300.0, true), mlp = Biquad::pass (sr, 2500.0, false);
                const float slow = envCoef (25.0), fast = envCoef (0.5);
                std::mt19937 rng (12345u);   // same noise on every channel: no stereo hiss
                float e = 0.0f, pf = 0.0f, ps = 0.0f;
                for (int i = 0; i < n; ++i)
                {
                    e = env[(size_t) i] + slow * (e - env[(size_t) i]);
                    const float r = std::abs (mlp (mhp (dry.getSample (ch, i))));
                    pf = r + fast * (pf - r);
                    ps = r + slow * (ps - r);
                    const float pulse = juce::jlimit (0.0f, 3.0f, pf / (ps + 1.0e-9f));
                    x[i] += st.breath * 0.16f * gate[(size_t) i] * e * pulse * bump (lp (hp2 (hp1 (uni (rng)))));
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
        // Where there is no voice (gaps: room tone, mic and interface hiss) the take is left as it
        // was: the make-up gain, EQ, grit and breath would otherwise lift the noise floor by
        // several dB, which is heard as hiss, more so after the compressors that follow.
        // The make-up gain g is solved so that the blend dry * (1 - gate) + g * wet * gate has
        // the take's loudness: sum (A + gB)^2 = sum dry^2, a quadratic in g.
        double aa = 0.0, ab = 0.0, bb = 0.0, target = 0.0;
        for (int ch = 0; ch < audio.getNumChannels(); ++ch)
        {
            const float* x = audio.getReadPointer (ch);
            const float* d = dry.getReadPointer (ch);
            for (int i = 0; i < n; ++i)
            {
                const double gi = gate[(size_t) i], A = d[i] * (1.0 - gi), B = x[i] * gi;
                aa += A * A; ab += A * B; bb += B * B; target += (double) d[i] * d[i];
            }
        }
        if (bb > 1.0e-18)
        {
            const double disc = ab * ab - bb * (aa - target);
            const double g = disc > 0.0 ? (-ab + std::sqrt (disc)) / bb : 1.0;
            const float knee = 0.9f, room = 0.09f;
            for (int ch = 0; ch < audio.getNumChannels(); ++ch)
            {
                float* x = audio.getWritePointer (ch);
                const float* d = dry.getReadPointer (ch);
                for (int i = 0; i < n; ++i)
                {
                    const float gi = gate[(size_t) i];
                    x[i] = d[i] * (1.0f - gi) + (float) g * x[i] * gi;
                    if (const float a = std::abs (x[i]); a > knee)
                        x[i] = std::copysign (knee + room * std::tanh ((a - knee) / room), x[i]);
                }
            }
        }
    }
}
