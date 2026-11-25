#include "PitchCorrectRenderer.h"
#include "Style/ExpressionLayer.h"

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
                                           double sr, const std::vector<float>& f0, const std::vector<float>& shift,
                                           const TimeWarp& warp)
    {
        const int n = in.getNumSamples();
        const int chs = in.getNumChannels();
        out.makeCopyOf (in);
        if (n == 0) return;

        const bool warped = ! warp.isIdentity();
        auto inputAt = [&] (double t) { return warped ? warp.inputAt (t) : t; };
        auto clampIdx = [n] (double t) { return juce::jlimit (0, n - 1, (int) t); };

        const double unvoicedPeriod = sr * 0.005;
        auto periodAt = [&] (double u)
        {
            const float f = f0[(size_t) clampIdx (u)];
            return f > 0.0f ? sr / (double) f : unvoicedPeriod;
        };

        // Output samples that differ from the input: shifted, or read from another time.
        std::vector<char> moved ((size_t) n, 0), shifted ((size_t) n, 0);
        for (int i = 0; i < n; ++i)
        {
            const double u = inputAt ((double) i);
            shifted[(size_t) i] = std::abs (shift[(size_t) clampIdx (u)]) > 0.0f ? 1 : 0;
            moved[(size_t) i] = warped && std::abs (u - (double) i) > 0.25 ? 1 : 0;
        }

        // Process those stretches plus a margin.
        std::vector<char> active ((size_t) n, 0);
        {
            const int margin = (int) (sr * 0.03);
            // Two sweeps give a symmetric dilation without the O(n * margin) loop.
            int lastOn = -margin - 1;
            for (int i = 0; i < n; ++i)
            {
                if (shifted[(size_t) i] || moved[(size_t) i]) lastOn = i;
                if (i - lastOn <= margin) active[(size_t) i] = 1;
            }
            int nextOn = n + margin + 1;
            for (int i = n - 1; i >= 0; --i)
            {
                if (shifted[(size_t) i] || moved[(size_t) i]) nextOn = i;
                if (nextOn - i <= margin) active[(size_t) i] = 1;
            }
        }

        // Analysis pitch marks across the whole take (input time). In voiced parts each mark
        // is snapped to the waveform peak near where the period says it should be, so grains
        // start at the same point of every glottal cycle; marks spaced by the period alone
        // drift against the waveform and the overlapping grains partly cancel (roughness,
        // "glitchy" pitch). Period jumps of more than 20% between marks (a tracker octave
        // error) are ignored.
        std::vector<float> mono ((size_t) n, 0.0f);
        {
            const int h = juce::jmax (1, (int) (sr * 0.0005));   // ~1 ms smoothing for peak picking
            std::vector<double> sum ((size_t) n + 1, 0.0);
            for (int i = 0; i < n; ++i)
            {
                double v = 0.0;
                for (int ch = 0; ch < chs; ++ch) v += in.getSample (ch, i);
                sum[(size_t) i + 1] = sum[(size_t) i] + v;
            }
            for (int i = 0; i < n; ++i)
            {
                const int a = juce::jmax (0, i - h), b = juce::jmin (n, i + h + 1);
                mono[(size_t) i] = (float) ((sum[(size_t) b] - sum[(size_t) a]) / (b - a));
            }
        }
        std::vector<double> marks;
        double lastT = unvoicedPeriod;
        for (double t = 0.0; t < n;)
        {
            marks.push_back (t);
            double T = periodAt (t);
            const bool voiced = f0[(size_t) clampIdx (t)] > 0.0f;
            if (voiced && lastT > 0.0 && (T > lastT * 1.2 || T < lastT / 1.2) && f0[(size_t) clampIdx (t - lastT)] > 0.0f)
                T = lastT;
            double next = t + T;
            if (voiced && f0[(size_t) clampIdx (next)] > 0.0f)
            {
                const int lo = clampIdx (next - 0.25 * T), hi = clampIdx (next + 0.25 * T);
                int best = clampIdx (next);
                for (int i = lo; i <= hi; ++i)
                    if (mono[(size_t) i] > mono[(size_t) best]) best = i;
                if (best > t + 0.5 * T) next = best;
            }
            lastT = juce::jmax (1.0, next - t);
            t = next;
        }

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

            // Synthesis marks in output time, spaced by the local period divided by the
            // pitch ratio; each takes the analysis grain nearest to where it reads from.
            size_t m = 0;
            for (double ts = seg; ts < segEnd;)
            {
                const double u = inputAt (ts);
                while (m + 1 < marks.size() && std::abs (marks[m + 1] - u) <= std::abs (marks[m] - u)) ++m;
                const double ta = marks[m];
                const double T = m + 1 < marks.size() ? juce::jlimit (8.0, sr * 0.02, marks[m + 1] - marks[m]) : periodAt (ta);
                const float sft = shift[(size_t) clampIdx (u)];
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
                ts += f0[(size_t) clampIdx (u)] > 0.0f ? T / ratio : T;
            }
            seg = segEnd;
        }

        // Crossfade mask: processed where shifted or moved, original elsewhere. The moved
        // part is dilated first so the fade happens where input and output line up again.
        const int fade = juce::jmax (1, (int) (sr * 0.01));
        auto smooth = [n, fade] (std::vector<float>& v)
        {
            std::vector<float> sm ((size_t) n, 0.0f);
            double run = 0.0;
            for (int i = 0; i < n + fade; ++i)
            {
                if (i < n) run += v[(size_t) i];
                if (i - 2 * fade >= 0) run -= v[(size_t) (i - 2 * fade)];
                const int c = i - fade;
                if (c >= 0 && c < n) sm[(size_t) c] = (float) (run / (2 * fade));
            }
            v.swap (sm);
        };
        std::vector<float> mix ((size_t) n, 0.0f);
        for (int i = 0; i < n; ++i) mix[(size_t) i] = shifted[(size_t) i] ? 1.0f : 0.0f;
        smooth (mix);
        if (warped)
        {
            std::vector<float> mv ((size_t) n, 0.0f);
            int lastOn = -fade - 1;
            for (int i = 0; i < n; ++i) { if (moved[(size_t) i]) lastOn = i; if (i - lastOn <= fade) mv[(size_t) i] = 1.0f; }
            int nextOn = n + fade + 1;
            for (int i = n - 1; i >= 0; --i) { if (moved[(size_t) i]) nextOn = i; if (nextOn - i <= fade) mv[(size_t) i] = 1.0f; }
            smooth (mv);
            for (int i = 0; i < n; ++i) mix[(size_t) i] = juce::jmax (mix[(size_t) i], mv[(size_t) i]);
        }

        for (int i = 0; i < n; ++i)
        {
            const float m = mix[(size_t) i];
            if (m <= 0.0f) continue;
            const bool hasGrains = wsum[(size_t) i] >= 1.0e-3f;
            const float inv = hasGrains ? 1.0f / wsum[(size_t) i] : 0.0f;
            for (int ch = 0; ch < chs; ++ch)
            {
                const float processed = hasGrains ? acc.getSample (ch, i) * inv : 0.0f;
                out.setSample (ch, i, (1.0f - m) * in.getSample (ch, i) + m * processed);
            }
        }
    }

    void PitchCorrectRenderer::resample (juce::AudioBuffer<float>& audio, double fromRate, double toRate)
    {
        if (toRate <= 0.0 || std::abs (toRate - fromRate) <= 0.5 || audio.getNumSamples() == 0)
            return;
        const double ratio = fromRate / toRate;
        const int n = audio.getNumSamples();
        const int outLen = (int) std::ceil ((double) n / ratio);
        juce::AudioBuffer<float> out (audio.getNumChannels(), outLen);
        for (int ch = 0; ch < audio.getNumChannels(); ++ch)
        {
            juce::LagrangeInterpolator interp;
            interp.process (ratio, audio.getReadPointer (ch), out.getWritePointer (ch), outLen, n, 0);
        }
        audio = std::move (out);
    }

    juce::Result PitchCorrectRenderer::renderAtSourceRate (const RegionDescriptor& region, const RenderPlan& plan, JobContext& ctx,
                                                           juce::AudioBuffer<float>& result, double& rate, std::vector<float>* plannedF0)
    {
        juce::AudioBuffer<float> audio;
        rate = 0.0;
        if (auto r = loadAudioFile (region.vocalFile, 0.0, &ctx, audio, rate); r.failed())
            return r;
        ctx.setProgress (0.1f);

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

        const auto shift = ExpressionLayer::buildShiftCurve (plan.notes, plan.style, f0, rate);
        const auto warp = TimeWarp::build (plan.notes, plan.style, region.tempoMap, n);
        psolaShift (audio, result, rate, f0, shift, warp);

        if (plannedF0 != nullptr)
        {
            // The pitch the output should have, in output time.
            plannedF0->assign ((size_t) n, 0.0f);
            for (int t = 0; t < n; ++t)
            {
                const int u = juce::jlimit (0, n - 1, (int) std::llround (warp.isIdentity() ? (double) t : warp.inputAt ((double) t)));
                const float f = f0[(size_t) u];
                (*plannedF0)[(size_t) t] = f > 0.0f ? f * std::pow (2.0f, shift[(size_t) u] / 12.0f) : 0.0f;
            }
        }
        return juce::Result::ok();
    }

    juce::Result PitchCorrectRenderer::render (const RegionDescriptor& region, const RenderPlan& plan, double targetRate,
                                               JobContext& ctx, RenderedAudio& out)
    {
        double rate = 0.0;
        juce::AudioBuffer<float> shifted;
        if (auto r = renderAtSourceRate (region, plan, ctx, shifted, rate, nullptr); r.failed())
            return r;
        ctx.setProgress (0.8f);
        if (ctx.shouldCancel()) return juce::Result::fail ("cancelled");

        resample (shifted, rate, targetRate);
        if (targetRate > 0.0) rate = targetRate;
        out.audio = std::move (shifted);
        out.sampleRate = rate;
        out.regionId = region.id;
        out.planHash = region.planHash;
        out.peaks = WaveformPeaks::compute (out.audio, rate);
        ctx.setProgress (1.0f);
        return juce::Result::ok();
    }
}
