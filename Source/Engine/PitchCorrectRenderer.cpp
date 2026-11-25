#include "PitchCorrectRenderer.h"

namespace ve
{
    std::vector<float> PitchCorrectRenderer::buildShiftCurve (const NotePlan& plan, int numSamples, double sr)
    {
        std::vector<float> shift ((size_t) numSamples, 0.0f);
        const int ramp = juce::jmax (1, (int) (0.015 * sr)); // 15 ms glide in/out of each note

        for (auto& n : plan.notes)
        {
            if (n.correction <= 0.0f) continue;
            const float amount = (n.targetMidi - n.detectedMidi) * juce::jlimit (0.0f, 1.0f, n.correction);
            if (std::abs (amount) < 1.0e-4f) continue;

            const int a = (int) juce::jlimit<std::int64_t> (0, numSamples, n.start);
            const int b = (int) juce::jlimit<std::int64_t> (0, numSamples, n.end);
            const int len = b - a;
            if (len <= 0) continue;
            const int r = juce::jmin (ramp, len / 2);
            for (int i = a; i < b; ++i)
            {
                float g = 1.0f;
                if (r > 0)
                {
                    if (i - a < r) g = (float) (i - a) / (float) r;
                    else if (b - 1 - i < r) g = (float) (b - 1 - i) / (float) r;
                }
                shift[(size_t) i] = amount * g;
            }
        }
        return shift;
    }

    void PitchCorrectRenderer::psolaShift (const juce::AudioBuffer<float>& in, juce::AudioBuffer<float>& out,
                                           double sr, const std::vector<float>& f0, const std::vector<float>& shift)
    {
        const int n = in.getNumSamples();
        const int chs = in.getNumChannels();
        out.makeCopyOf (in);
        if (n == 0) return;

        const double unvoicedPeriod = sr * 0.005;
        auto periodAt = [&] (double t)
        {
            const int i = juce::jlimit (0, n - 1, (int) t);
            return f0[(size_t) i] > 0.0f ? sr / (double) f0[(size_t) i] : unvoicedPeriod;
        };

        // Only process stretches that are actually shifted (plus a margin).
        std::vector<char> active ((size_t) n, 0);
        const int margin = (int) (sr * 0.03);
        for (int i = 0; i < n; ++i)
            if (std::abs (shift[(size_t) i]) > 0.0f)
                for (int j = juce::jmax (0, i - margin); j <= juce::jmin (n - 1, i + margin); ++j) active[(size_t) j] = 1;

        // Analysis pitch marks across the whole take.
        std::vector<double> marks;
        for (double t = 0.0; t < n; t += periodAt (t))
            marks.push_back (t);

        juce::AudioBuffer<float> acc (chs, n);
        acc.clear();
        std::vector<float> wsum ((size_t) n, 0.0f);

        int seg = 0;
        while (seg < n)
        {
            while (seg < n && ! active[(size_t) seg]) ++seg;
            if (seg >= n) break;
            int segEnd = seg;
            while (segEnd < n && active[(size_t) segEnd]) ++segEnd;

            // Synthesis marks: spaced by the analysis period divided by the pitch ratio.
            size_t m = 0;
            for (double ts = seg; ts < segEnd;)
            {
                while (m + 1 < marks.size() && std::abs (marks[m + 1] - ts) <= std::abs (marks[m] - ts)) ++m;
                const double ta = marks[m];
                const double T = periodAt (ta);
                const float sft = shift[(size_t) juce::jlimit (0, n - 1, (int) ts)];
                const double ratio = std::pow (2.0, (double) sft / 12.0);

                // Hann grain of length 2T centred on the analysis mark, placed at ts.
                const int half = juce::jmax (2, (int) std::ceil (T));
                for (int k = -half; k <= half; ++k)
                {
                    const int src = (int) std::llround (ta) + k;
                    const int dst = (int) std::llround (ts) + k;
                    if (src < 0 || src >= n || dst < 0 || dst >= n) continue;
                    const float w = 0.5f + 0.5f * (float) std::cos (juce::MathConstants<double>::pi * k / (double) half);
                    for (int ch = 0; ch < chs; ++ch)
                        acc.addSample (ch, dst, in.getSample (ch, src) * w);
                    wsum[(size_t) dst] += w;
                }
                ts += f0[(size_t) juce::jlimit (0, n - 1, (int) ts)] > 0.0f ? T / ratio : T;
            }
            seg = segEnd;
        }

        // Normalise and crossfade: shifted where the curve is non-zero, original elsewhere.
        const int fade = juce::jmax (1, (int) (sr * 0.01));
        std::vector<float> mix ((size_t) n, 0.0f);
        for (int i = 0; i < n; ++i) mix[(size_t) i] = std::abs (shift[(size_t) i]) > 0.0f ? 1.0f : 0.0f;
        // Smooth the mask with a moving window so joins don't click.
        {
            std::vector<float> sm ((size_t) n, 0.0f);
            double run = 0.0;
            for (int i = 0; i < n + fade; ++i)
            {
                if (i < n) run += mix[(size_t) i];
                if (i - 2 * fade >= 0) run -= mix[(size_t) (i - 2 * fade)];
                const int c = i - fade;
                if (c >= 0 && c < n) sm[(size_t) c] = (float) (run / (2 * fade));
            }
            mix.swap (sm);
        }

        for (int i = 0; i < n; ++i)
        {
            const float m = mix[(size_t) i];
            if (m <= 0.0f || wsum[(size_t) i] < 1.0e-3f) continue;
            const float inv = 1.0f / wsum[(size_t) i];
            for (int ch = 0; ch < chs; ++ch)
            {
                const float shifted = acc.getSample (ch, i) * inv;
                out.setSample (ch, i, (1.0f - m) * in.getSample (ch, i) + m * shifted);
            }
        }
    }

    juce::Result PitchCorrectRenderer::render (const RegionDescriptor& region, const RenderPlan& plan, double targetRate,
                                               JobContext& ctx, RenderedAudio& out)
    {
        juce::AudioBuffer<float> audio;
        double rate = 0.0;
        if (auto r = loadAudioFile (region.vocalFile, 0.0, &ctx, audio, rate); r.failed())
            return r;
        ctx.setProgress (0.3f);

        juce::MemoryBlock mb;
        PitchTrack track;
        if (! region.pitchFile.loadFileAsData (mb) || ! PitchTrack::deserialise (mb, track))
            return juce::Result::fail ("Missing pitch track for take " + region.id);

        // Per-sample f0 (linear between frames, 0 where unvoiced).
        const int n = audio.getNumSamples();
        std::vector<float> f0 ((size_t) n, 0.0f);
        for (int i = 0; i < n; ++i)
        {
            const double fi = (double) i / track.hopSize;
            const int a = juce::jlimit (0, (int) track.frames.size() - 1, (int) fi);
            const int b = juce::jmin (a + 1, (int) track.frames.size() - 1);
            const float fa = track.frames[(size_t) a].f0, fb = track.frames[(size_t) b].f0;
            if (fa > 0.0f && fb > 0.0f) f0[(size_t) i] = fa + (fb - fa) * (float) (fi - a);
            else f0[(size_t) i] = fa > 0.0f ? fa : fb;
        }
        if (ctx.shouldCancel()) return juce::Result::fail ("cancelled");

        const auto shift = buildShiftCurve (plan.notes, n, rate);
        juce::AudioBuffer<float> shifted;
        psolaShift (audio, shifted, rate, f0, shift);
        ctx.setProgress (0.8f);
        if (ctx.shouldCancel()) return juce::Result::fail ("cancelled");

        // Resample to the engine rate if needed.
        if (targetRate > 0.0 && std::abs (targetRate - rate) > 0.5)
        {
            const double ratio = rate / targetRate;
            const int outLen = (int) std::ceil ((double) n / ratio);
            out.audio.setSize (shifted.getNumChannels(), outLen);
            for (int ch = 0; ch < shifted.getNumChannels(); ++ch)
            {
                juce::LagrangeInterpolator interp;
                interp.process (ratio, shifted.getReadPointer (ch), out.audio.getWritePointer (ch), outLen, n, 0);
            }
            rate = targetRate;
        }
        else
        {
            out.audio = std::move (shifted);
        }

        out.sampleRate = rate;
        out.regionId = region.id;
        out.planHash = region.planHash;
        out.peaks = WaveformPeaks::compute (out.audio, rate);
        ctx.setProgress (1.0f);
        return juce::Result::ok();
    }
}
