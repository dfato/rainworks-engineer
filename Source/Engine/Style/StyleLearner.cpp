#include "StyleLearner.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>

namespace ve::StyleLearner
{
    namespace
    {
        constexpr float nan = std::numeric_limits<float>::quiet_NaN();

        float median (std::vector<float> v)
        {
            v.erase (std::remove_if (v.begin(), v.end(), [] (float x) { return ! std::isfinite (x); }), v.end());
            if (v.empty()) return 0.0f;
            const auto mid = v.begin() + (std::ptrdiff_t) (v.size() / 2);
            std::nth_element (v.begin(), mid, v.end());
            if (v.size() % 2 == 1) return *mid;
            const float hi = *mid;
            return 0.5f * (hi + *std::max_element (v.begin(), mid));
        }

        // Cents per frame (MIDI * 100), NaN where unvoiced. Gaps up to maxGap frames are bridged.
        std::vector<float> centsTrack (const PitchTrack& t, int maxGap)
        {
            std::vector<float> c (t.frames.size(), nan);
            for (size_t i = 0; i < c.size(); ++i)
                if (t.frames[i].f0 > 0.0f) c[i] = 100.0f * PitchTrack::hzToMidi (t.frames[i].f0);

            int last = -1;
            for (int i = 0; i < (int) c.size(); ++i)
            {
                if (! std::isfinite (c[(size_t) i])) continue;
                if (last >= 0 && i - last > 1 && i - last - 1 <= maxGap)
                    for (int j = last + 1; j < i; ++j)
                        c[(size_t) j] = c[(size_t) last] + (c[(size_t) i] - c[(size_t) last]) * (float) (j - last) / (float) (i - last);
                last = i;
            }
            return c;
        }

        // Centred moving average over [lo, hi) of x, ignoring NaN, half-width h frames.
        std::vector<float> movingAverage (const std::vector<float>& x, int lo, int hi, int h)
        {
            std::vector<float> out ((size_t) juce::jmax (0, hi - lo), nan);
            for (int i = lo; i < hi; ++i)
            {
                double s = 0.0; int n = 0;
                for (int j = juce::jmax (lo, i - h); j < juce::jmin (hi, i + h + 1); ++j)
                    if (std::isfinite (x[(size_t) j])) { s += x[(size_t) j]; ++n; }
                if (n > 0) out[(size_t) (i - lo)] = (float) (s / n);
            }
            return out;
        }

        float wrapCents (float c) // to (-50, 50]
        {
            c = std::fmod (c, 100.0f);
            if (c > 50.0f) c -= 100.0f;
            if (c <= -50.0f) c += 100.0f;
            return c;
        }

        struct VibratoResult { bool present = false; float rateHz = 0, depthCents = 0, onsetMs = 0; };

        // r: pitch deviation in cents (no NaN) of a held note after its attack.
        VibratoResult detectVibrato (const std::vector<float>& dev, float frameMs, float offsetMs)
        {
            VibratoResult v;
            const int n = (int) dev.size();
            const int h = juce::jmax (1, (int) std::round (110.0f / frameMs));
            if (n < 4 * h) return v;

            std::vector<float> r ((size_t) n);
            const auto trend = movingAverage (dev, 0, n, h);
            double energy = 0.0;
            for (int i = 0; i < n; ++i) { r[(size_t) i] = dev[(size_t) i] - trend[(size_t) i]; energy += (double) r[(size_t) i] * r[(size_t) i]; }
            if (energy <= 0.0) return v;
            const float rms = (float) std::sqrt (energy / n);

            const int minLag = juce::jmax (2, (int) std::floor (1000.0f / (9.0f * frameMs)));
            const int maxLag = juce::jmin (n / 2, (int) std::ceil (1000.0f / (3.5f * frameMs)));
            if (maxLag <= minLag + 1) return v;

            auto acf = [&] (int lag)
            {
                double s = 0.0;
                for (int i = 0; i + lag < n; ++i) s += (double) r[(size_t) i] * r[(size_t) (i + lag)];
                return (float) (s / energy * n / (double) (n - lag));
            };

            int best = -1; float bestVal = -1.0f;
            std::vector<float> a ((size_t) (maxLag + 2), 0.0f);
            for (int lag = minLag - 1; lag <= maxLag + 1; ++lag) a[(size_t) lag] = acf (lag);
            for (int lag = minLag; lag <= maxLag; ++lag)
                if (a[(size_t) lag] > a[(size_t) (lag - 1)] && a[(size_t) lag] >= a[(size_t) (lag + 1)] && a[(size_t) lag] > bestVal)
                    { bestVal = a[(size_t) lag]; best = lag; }

            if (best < 0 || bestVal < 0.35f || rms < 6.0f) return v;

            const float y0 = a[(size_t) (best - 1)], y1 = a[(size_t) best], y2 = a[(size_t) (best + 1)];
            const float denom = y0 - 2.0f * y1 + y2;
            const float frac = std::abs (denom) > 1e-9f ? juce::jlimit (-0.5f, 0.5f, 0.5f * (y0 - y2) / denom) : 0.0f;

            v.present = true;
            v.rateHz = 1000.0f / (((float) best + frac) * frameMs);
            v.depthCents = rms * std::sqrt (2.0f);

            // Onset: first point where the running amplitude reaches half the note's depth.
            const int period = juce::jmax (2, best);
            for (int i = 0; i + period <= n; ++i)
            {
                double s = 0.0;
                for (int j = i; j < i + period; ++j) s += (double) r[(size_t) j] * r[(size_t) j];
                if (std::sqrt (2.0 * s / period) >= 0.5 * v.depthCents)
                {
                    v.onsetMs = offsetMs + ((float) i + 0.5f * (float) period) * frameMs;  // window centre
                    break;
                }
            }
            return v;
        }

        float trustFrom (int count, float k) { return count <= 0 ? 0.0f : (float) count / ((float) count + k); }

        juce::var obj (std::initializer_list<std::pair<const char*, juce::var>> kv)
        {
            auto* o = new juce::DynamicObject();
            for (auto& [k, val] : kv) o->setProperty (k, val);
            return juce::var (o);
        }

        double round2 (double x) { return std::round (x * 100.0) / 100.0; }
    }

    //==============================================================================
    juce::var TakeStats::toVar() const
    {
        return obj ({
            { "voicedSeconds", round2 (voicedSeconds) }, { "notes", notes },
            { "tuningOffsetCents", round2 (tuningOffsetCents) },
            { "inTuneCents", round2 (inTuneCents) }, { "inTuneNotes", inTuneNotes },
            { "settleMs", round2 (settleMs) }, { "settleNotes", settleNotes },
            { "wanderCents", round2 (wanderCents) }, { "wanderNotes", wanderNotes },
            { "longNotes", longNotes }, { "vibratoNotes", vibratoNotes },
            { "vibratoRateHz", round2 (vibratoRateHz) }, { "vibratoDepthCents", round2 (vibratoDepthCents) }, { "vibratoOnsetMs", round2 (vibratoOnsetMs) },
            { "scoopCandidates", scoopCandidates }, { "scoopedNotes", scoopedNotes },
            { "scoopCents", round2 (scoopCents) }, { "scoopMs", round2 (scoopMs) },
            { "transitions", transitions }, { "transitionMs", round2 (transitionMs) },
            { "timingValid", timingValid }, { "timedOnsets", timedOnsets }, { "gridLock", round2 (gridLock) },
            { "onsetSpreadMs", round2 (onsetSpreadMs) }, { "swing", round2 (swing) }, { "offbeatOnsets", offbeatOnsets },
            { "techniqueSeconds", round2 (techniqueSeconds) }, { "gritShare", round2 (gritShare) },
            { "breathyShare", round2 (breathyShare) }, { "beltShare", round2 (beltShare) }, { "spokenShare", round2 (spokenShare) },
        });
    }

    TakeStats TakeStats::fromVar (const juce::var& v)
    {
        TakeStats s;
        auto f = [&] (const char* k, float d) { auto x = v.getProperty (k, {}); return x.isVoid() ? d : (float) (double) x; };
        auto i = [&] (const char* k) { return (int) v.getProperty (k, 0); };
        s.voicedSeconds = f ("voicedSeconds", 0); s.notes = i ("notes");
        s.tuningOffsetCents = f ("tuningOffsetCents", 0);
        s.inTuneCents = f ("inTuneCents", 0); s.inTuneNotes = i ("inTuneNotes");
        s.settleMs = f ("settleMs", 0); s.settleNotes = i ("settleNotes");
        s.wanderCents = f ("wanderCents", 0); s.wanderNotes = i ("wanderNotes");
        s.longNotes = i ("longNotes"); s.vibratoNotes = i ("vibratoNotes");
        s.vibratoRateHz = f ("vibratoRateHz", 0); s.vibratoDepthCents = f ("vibratoDepthCents", 0); s.vibratoOnsetMs = f ("vibratoOnsetMs", 0);
        s.scoopCandidates = i ("scoopCandidates"); s.scoopedNotes = i ("scoopedNotes");
        s.scoopCents = f ("scoopCents", 0); s.scoopMs = f ("scoopMs", 0);
        s.transitions = i ("transitions"); s.transitionMs = f ("transitionMs", 0);
        s.timingValid = (bool) v.getProperty ("timingValid", false); s.timedOnsets = i ("timedOnsets");
        s.gridLock = f ("gridLock", 0); s.onsetSpreadMs = f ("onsetSpreadMs", 0); s.swing = f ("swing", 0);
        s.offbeatOnsets = i ("offbeatOnsets");
        s.techniqueSeconds = f ("techniqueSeconds", 0);
        s.gritShare = f ("gritShare", 0); s.breathyShare = f ("breathyShare", 0);
        s.beltShare = f ("beltShare", 0); s.spokenShare = f ("spokenShare", 0);
        return s;
    }

    //==============================================================================
    TakeStats analyse (const PitchTrack& track, const std::vector<Note>& notes, double bpm)
    {
        TakeStats st;
        if (track.frames.empty() || track.sampleRate <= 0.0 || track.hopSize <= 0) return st;

        const float fm = (float) (1000.0 * track.hopSize / track.sampleRate);   // ms per frame
        const int nf = (int) track.frames.size();
        const auto cents = centsTrack (track, juce::jmax (1, (int) (30.0f / fm)));
        auto frames = [&] (float ms) { return juce::jmax (1, (int) std::round (ms / fm)); };

        for (auto& fr : track.frames) if (fr.f0 > 0.0f) st.voicedSeconds += fm / 1000.0;
        st.notes = (int) notes.size();

        // Whole-take tuning offset (circular mean of note centres), so a take sung a little
        // sharp of A440 throughout is not counted as out of tune.
        {
            std::complex<double> z {};
            for (auto& n : notes)
                if ((double) (n.end - n.start) * 1000.0 / track.sampleRate >= 150.0)
                    z += std::polar (1.0, juce::MathConstants<double>::twoPi * (double) wrapCents (n.detectedMidi * 100.0f) / 100.0);
            st.tuningOffsetCents = std::abs (z) > 0.0 ? (float) (std::arg (z) / juce::MathConstants<double>::twoPi * 100.0) : 0.0f;
        }

        std::vector<float> inTune, settle, wander, vibRate, vibDepth, vibOnset, scoopDepth, scoopLen, trans;
        std::vector<char> vibratoNote (notes.size(), 0);

        for (size_t k = 0; k < notes.size(); ++k)
        {
            const auto& n = notes[k];
            const int a = (int) juce::jlimit<std::int64_t> (0, nf, n.start / track.hopSize);
            const int b = (int) juce::jlimit<std::int64_t> (0, nf, n.end / track.hopSize);
            if (b - a < 3) continue;
            const float durMs = (float) (b - a) * fm;
            const float centre = n.detectedMidi * 100.0f;

            int voiced = 0;
            for (int i = a; i < b; ++i) if (std::isfinite (cents[(size_t) i])) ++voiced;
            const float voicedFrac = (float) voiced / (float) (b - a);
            if (voicedFrac < 0.6f) continue;

            auto dev = [&] (int i) { return cents[(size_t) i] - centre; };

            if (durMs >= 150.0f)
            {
                inTune.push_back (std::abs (wrapCents (centre - st.tuningOffsetCents)));
            }

            // Settle: first frame where the next 40 ms average within 30 cents of the centre.
            float settleMs = -1.0f;
            if (durMs >= 200.0f)
            {
                const int w = frames (40.0f);
                const int limit = juce::jmin (b - w, a + (int) (juce::jmin (300.0f, durMs * 0.6f) / fm));
                for (int i = a; i <= limit; ++i)
                {
                    double s = 0.0; int c = 0;
                    for (int j = i; j < i + w; ++j) if (std::isfinite (cents[(size_t) j])) { s += dev (j); ++c; }
                    if (c > 0 && std::abs (s / c) < 30.0) { settleMs = (float) (i - a) * fm; break; }
                }
                if (settleMs >= 0.0f) settle.push_back (settleMs);
            }

            // Scoop: an attack from silence (or from above) that starts clearly below the note.
            const float gapMs = k > 0 ? (float) ((double) (n.start - notes[k - 1].end) * 1000.0 / track.sampleRate) : 1.0e9f;
            const bool fromBelow = k > 0 && gapMs <= 80.0f && notes[k - 1].detectedMidi < n.detectedMidi - 0.3f;
            if (durMs >= 150.0f && ! fromBelow && settleMs >= 0.0f)
            {
                // Skip the first 10 ms (consonant/attack frames are unreliable), then
                // average 20 ms. A scoop must also be rising and not an octave-ish jump.
                double s = 0.0; int c = 0;
                const int from = a + frames (10.0f), to = juce::jmin (b, from + frames (20.0f));
                for (int i = from; i < to; ++i)
                    if (std::isfinite (cents[(size_t) i])) { s += dev (i); ++c; }
                const int later = juce::jmin (b - 1, to + frames (20.0f));
                if (c > 0 && std::isfinite (cents[(size_t) later]))
                {
                    ++st.scoopCandidates;
                    const float initial = (float) (s / c);
                    const bool rising = dev (later) > initial + 15.0f;
                    if (initial < -35.0f && initial > -250.0f && rising && settleMs > fm)
                    {
                        // Length: until the pitch is within 10 cents of the note (or turns down).
                        int end = a;
                        const int limit = juce::jmin (b, a + frames (200.0f));
                        while (end < limit && ! (std::isfinite (cents[(size_t) end]) && dev (end) >= -10.0f)) ++end;
                        ++st.scoopedNotes;
                        scoopDepth.push_back (-initial);
                        scoopLen.push_back ((float) (end - a) * fm);
                    }
                }
            }

            // Vibrato on long, well-voiced notes (after the attack, before the release).
            if (durMs >= 450.0f && voicedFrac >= 0.85f)
            {
                ++st.longNotes;
                const int from = a + frames (juce::jmax (80.0f, settleMs));
                const int to = b - frames (30.0f);
                std::vector<float> d;
                for (int i = from; i < to; ++i) d.push_back (std::isfinite (cents[(size_t) i]) ? dev (i) : (d.empty() ? 0.0f : d.back()));
                const auto v = detectVibrato (d, fm, (float) (from - a) * fm);
                if (v.present)
                {
                    ++st.vibratoNotes;
                    vibratoNote[k] = 1;
                    vibRate.push_back (v.rateHz);
                    vibDepth.push_back (v.depthCents);
                    vibOnset.push_back (v.onsetMs);
                }
            }

            // Wander: pitch spread on held notes without vibrato, after the attack.
            if (durMs >= 250.0f && ! vibratoNote[k] && settleMs >= 0.0f)
            {
                const int from = a + frames (juce::jmax (settleMs, 30.0f));
                const int to = b - frames (30.0f);
                double s = 0.0, s2 = 0.0; int c = 0;
                for (int i = from; i < to; ++i)
                    if (std::isfinite (cents[(size_t) i])) { const double d = dev (i); s += d; s2 += d * d; ++c; }
                if ((float) c * fm >= 100.0f)
                {
                    const double mean = s / c;
                    wander.push_back ((float) std::sqrt (juce::jmax (0.0, s2 / c - mean * mean)));
                }
            }
        }

        // Transitions between connected notes: 10%..90% of the pitch move.
        for (size_t k = 0; k + 1 < notes.size(); ++k)
        {
            const auto& x = notes[k];
            const auto& y = notes[k + 1];
            const float gapMs = (float) ((double) (y.start - x.end) * 1000.0 / track.sampleRate);
            const float interval = (y.detectedMidi - x.detectedMidi) * 100.0f;
            if (gapMs > 60.0f || std::abs (interval) < 80.0f || std::abs (interval) > 1200.0f) continue;

            const int join = (int) juce::jlimit<std::int64_t> (0, nf - 1, x.end / track.hopSize);
            const int lo = (int) juce::jmax<std::int64_t> (x.start / track.hopSize, join - frames (200.0f));
            const int hi = (int) juce::jmin<std::int64_t> (juce::jmin<std::int64_t> (nf, y.end / track.hopSize), join + frames (200.0f));
            if (hi - lo < 6) continue;
            const auto sm = movingAverage (cents, lo, hi, 2);
            auto progress = [&] (int i) { const float c = sm[(size_t) (i - lo)]; return std::isfinite (c) ? (c - x.detectedMidi * 100.0f) / interval : nan; };

            int t0 = -1, t1 = -1;
            for (int i = juce::jmin (join, hi - 1); i >= lo; --i) { const float p = progress (i); if (std::isfinite (p) && p <= 0.1f) { t0 = i; break; } }
            for (int i = juce::jmax (join, lo); i < hi; ++i)      { const float p = progress (i); if (std::isfinite (p) && p >= 0.9f) { t1 = i; break; } }
            if (t0 < 0 || t1 < 0 || t1 <= t0) continue;
            trans.push_back ((float) (t1 - t0) * fm);
        }

        st.inTuneNotes = (int) inTune.size();   st.inTuneCents = median (inTune);
        st.settleNotes = (int) settle.size();   st.settleMs = median (settle);
        st.wanderNotes = (int) wander.size();   st.wanderCents = median (wander);
        st.vibratoRateHz = median (vibRate);    st.vibratoDepthCents = median (vibDepth); st.vibratoOnsetMs = median (vibOnset);
        st.scoopCents = median (scoopDepth);    st.scoopMs = median (scoopLen);
        st.transitions = (int) trans.size();    st.transitionMs = median (trans);

        // How the voice is produced, from the technique classifier.
        if (track.hasTechnique())
        {
            int voiced = 0, grit = 0, breathy = 0, belt = 0, spoken = 0;
            for (int i = 0; i < nf; ++i)
            {
                if (track.frames[(size_t) i].f0 <= 0.0f) continue;
                const auto& t = track.technique[(size_t) i];
                ++voiced;
                grit += t[TechniqueFrame::grit] >= 0.5f;
                breathy += t[TechniqueFrame::breathy] >= 0.5f;
                belt += t[TechniqueFrame::belt] >= 0.5f;
                spoken += t[TechniqueFrame::spoken] >= 0.5f;
            }
            if (voiced > 0)
            {
                st.techniqueSeconds = (float) voiced * fm / 1000.0f;
                st.gritShare = (float) grit / (float) voiced;
                st.breathyShare = (float) breathy / (float) voiced;
                st.beltShare = (float) belt / (float) voiced;
                st.spokenShare = (float) spoken / (float) voiced;
            }
        }

        // Timing against a 16th grid with optional swing. The grid phase and the exact
        // tempo (within +-1% of the given one) are searched, since a reference file
        // rarely starts exactly on a downbeat.
        if (bpm > 0.0)
        {
            std::vector<double> onsets;
            for (auto& n : notes)
                if ((double) (n.end - n.start) * 1000.0 / track.sampleRate >= 80.0)
                    onsets.push_back ((double) n.start / track.sampleRate);

            if (onsets.size() >= 16)
            {
                struct Fit { double score = -1, bpm = 0, phase = 0, swing = 0; } best;
                const double sigma = 0.015;
                for (int bk = -10; bk <= 10; ++bk)
                {
                    const double b = bpm * (1.0 + bk * 0.001);
                    const double step = 60.0 / b / 4.0, period = 2.0 * step;
                    for (int sk = 0; sk <= 10; ++sk)
                    {
                        const double off = step * (1.0 + (sk / 10.0) / 3.0);
                        for (double ph = 0.0; ph < period; ph += 0.002)
                        {
                            double score = 0.0;
                            for (double t : onsets)
                            {
                                double u = std::fmod (t - ph, period); if (u < 0) u += period;
                                const double d = juce::jmin (u, period - u, std::abs (u - off));
                                score += std::exp (-0.5 * (d / sigma) * (d / sigma));
                            }
                            if (score > best.score) best = { score, b, ph, sk / 10.0 };
                        }
                    }
                }

                const double step = 60.0 / best.bpm / 4.0, period = 2.0 * step;
                const double off = step * (1.0 + best.swing / 3.0);
                int near = 0, offbeat = 0;
                std::vector<float> spread;
                for (double t : onsets)
                {
                    double u = std::fmod (t - best.phase, period); if (u < 0) u += period;
                    const double dOn = juce::jmin (u, period - u), dOff = std::abs (u - off);
                    const double d = juce::jmin (dOn, dOff);
                    if (d <= 0.2 * step) ++near;
                    if (dOff < dOn) ++offbeat;
                    if (d < 0.5 * step) spread.push_back ((float) (d * 1000.0));
                }
                // Random onsets land within +-0.2 step of one of two grid points 40% of the time.
                const float frac = (float) near / (float) onsets.size();
                st.timedOnsets = (int) onsets.size();
                st.gridLock = juce::jlimit (0.0f, 1.0f, (frac - 0.4f) / 0.6f);
                st.onsetSpreadMs = median (spread);
                st.offbeatOnsets = offbeat;
                st.swing = offbeat >= 8 ? (float) best.swing : 0.0f;
                st.timingValid = st.gridLock >= 0.25f;
            }
        }
        return st;
    }

    TakeStats analyseAudio (const juce::AudioBuffer<float>& audio, double sampleRate, double bpm, JobContext* ctx,
                            PitchTracker::Engine engine)
    {
        PitchTrack track;
        if (PitchTracker::trackBest (audio, sampleRate, {}, ctx, track, engine).failed()) return {};
        const auto notes = NoteSegmenter::segment (track, {});
        return analyse (track, notes, bpm);
    }

    //==============================================================================
    TakeStats combine (const std::vector<TakeStats>& takes)
    {
        if (takes.size() == 1) return takes.front();
        TakeStats out;

        auto pool = [&] (auto countOf, auto valueOf, float& value, int& count)
        {
            std::vector<float> vals;
            count = 0;
            for (auto& t : takes)
                if (countOf (t) > 0) { vals.push_back (valueOf (t)); count += countOf (t); }
            value = median (vals);
        };

        for (auto& t : takes) { out.voicedSeconds += t.voicedSeconds; out.notes += t.notes; out.longNotes += t.longNotes;
                                out.scoopCandidates += t.scoopCandidates; }

        pool ([] (auto& t) { return t.inTuneNotes; }, [] (auto& t) { return t.inTuneCents; }, out.inTuneCents, out.inTuneNotes);
        pool ([] (auto& t) { return t.settleNotes; }, [] (auto& t) { return t.settleMs; }, out.settleMs, out.settleNotes);
        pool ([] (auto& t) { return t.wanderNotes; }, [] (auto& t) { return t.wanderCents; }, out.wanderCents, out.wanderNotes);
        int dummy = 0;
        pool ([] (auto& t) { return t.vibratoNotes; }, [] (auto& t) { return t.vibratoRateHz; }, out.vibratoRateHz, out.vibratoNotes);
        pool ([] (auto& t) { return t.vibratoNotes; }, [] (auto& t) { return t.vibratoDepthCents; }, out.vibratoDepthCents, dummy);
        pool ([] (auto& t) { return t.vibratoNotes; }, [] (auto& t) { return t.vibratoOnsetMs; }, out.vibratoOnsetMs, dummy);
        pool ([] (auto& t) { return t.scoopedNotes; }, [] (auto& t) { return t.scoopCents; }, out.scoopCents, out.scoopedNotes);
        pool ([] (auto& t) { return t.scoopedNotes; }, [] (auto& t) { return t.scoopMs; }, out.scoopMs, dummy);
        pool ([] (auto& t) { return t.transitions; }, [] (auto& t) { return t.transitionMs; }, out.transitionMs, out.transitions);

        {
            std::vector<float> g, b, bl, sp;
            for (auto& t : takes)
                if (t.techniqueSeconds > 0.0f)
                {
                    out.techniqueSeconds += t.techniqueSeconds;
                    g.push_back (t.gritShare); b.push_back (t.breathyShare); bl.push_back (t.beltShare); sp.push_back (t.spokenShare);
                }
            out.gritShare = median (g); out.breathyShare = median (b); out.beltShare = median (bl); out.spokenShare = median (sp);
        }

        std::vector<float> lock, spread, swing;
        for (auto& t : takes)
            if (t.timingValid)
            {
                out.timingValid = true;
                out.timedOnsets += t.timedOnsets;
                out.offbeatOnsets += t.offbeatOnsets;
                lock.push_back (t.gridLock);
                spread.push_back (t.onsetSpreadMs);
                if (t.offbeatOnsets >= 8) swing.push_back (t.swing);
            }
        out.gridLock = median (lock);
        out.onsetSpreadMs = median (spread);
        out.swing = median (swing);
        return out;
    }

    //==============================================================================
    Suggestion suggest (const TakeStats& s, int takeCount)
    {
        Suggestion g;
        auto& p = g.values;
        // More singers = more trust that this is the genre and not one person's habit.
        const float takes = takeCount <= 0 ? 0.0f : (float) takeCount / ((float) takeCount + 1.0f);

        p.correction    = juce::jlimit (0.3f, 1.0f, 1.1f - s.inTuneCents / 25.0f);
        // How fast the singer moves between notes is the best guide to how fast a
        // correction can pull without sounding unlike the genre (note starts are found
        // after the pitch has arrived, so per-note settle time reads short for everyone).
        p.retuneMs      = juce::jlimit (0.0f, 120.0f, s.transitionMs - 20.0f);
        p.keepVariation = juce::jlimit (0.05f, 1.0f, (s.wanderCents - 2.0f) / 12.0f);
        g.pitchTrust    = takes * trustFrom (juce::jmin (s.inTuneNotes, s.transitions), 40.0f);

        const float prevalence = s.longNotes > 0 ? (float) s.vibratoNotes / (float) s.longNotes : 0.0f;
        p.vibratoDepthCents = juce::jlimit (0.0f, 60.0f, s.vibratoDepthCents * prevalence * 0.6f);
        p.vibratoRateHz     = juce::jlimit (3.5f, 8.0f, s.vibratoRateHz > 0.0f ? s.vibratoRateHz : 5.5f);
        p.vibratoOnsetMs    = juce::jlimit (100.0f, 800.0f, s.vibratoOnsetMs > 0.0f ? s.vibratoOnsetMs : 300.0f);
        // Depth evidence includes "long notes without vibrato", so it can be trusted before rate is.
        g.vibratoTrust = takes * trustFrom (s.longNotes, 15.0f);

        const float scoopRate = s.scoopCandidates > 0 ? (float) s.scoopedNotes / (float) s.scoopCandidates : 0.0f;
        p.scoopCents = juce::jlimit (0.0f, 120.0f, s.scoopCents * juce::jmin (1.0f, scoopRate / 0.4f));
        p.scoopMs    = p.scoopCents > 0.0f ? juce::jlimit (20.0f, 150.0f, s.scoopMs) : 0.0f;
        g.scoopTrust = takes * trustFrom (s.scoopCandidates, 30.0f);

        p.slideMs   = juce::jlimit (0.0f, 200.0f, s.transitionMs / 0.8f);
        g.slideTrust = takes * trustFrom (s.transitions, 20.0f);

        p.quantize = juce::jlimit (0.0f, 0.6f, 0.6f - s.onsetSpreadMs / 50.0f);
        p.swing    = juce::jlimit (0.0f, 1.0f, s.swing);
        g.timingTrust = s.timingValid ? takes * trustFrom (s.timedOnsets, 60.0f) * juce::jlimit (0.0f, 1.0f, s.gridLock * 1.5f) : 0.0f;

        // Tone from technique: a style learned from gritty references adds rasp, one learned
        // from airy references adds breath. Shares are of voiced time.
        p.grit   = juce::jlimit (0.0f, 0.7f, s.gritShare * 1.2f);
        p.breath = juce::jlimit (0.0f, 0.5f, s.breathyShare * 0.8f);
        g.toneTrust = s.techniqueSeconds > 0.0f ? takes * trustFrom ((int) s.techniqueSeconds, 20.0f) : 0.0f;
        return g;
    }

    static constexpr float minCorrection = 0.8f;

    StyleProfile learn (const StyleProfile& base, const TakeStats& stats, int takeCount, float strength)
    {
        const auto g = suggest (stats, takeCount);
        const float k = juce::jlimit (0.0f, 1.0f, strength);
        auto mix = [k] (float from, float to, float trust) { return from + k * trust * (to - from); };

        StyleProfile p = base;
        // Correct never drops below 80%: references sung loosely make a style looser in how
        // it moves (retune, kept variation), not a take that stays out of tune.
        p.correction        = juce::jmax (minCorrection, mix (base.correction, g.values.correction, g.pitchTrust));
        p.retuneMs          = mix (base.retuneMs, g.values.retuneMs, g.pitchTrust);
        p.keepVariation     = mix (base.keepVariation, g.values.keepVariation, g.pitchTrust);
        p.vibratoDepthCents = mix (base.vibratoDepthCents, g.values.vibratoDepthCents, g.vibratoTrust);
        if (stats.vibratoNotes > 0)
        {
            const float rateTrust = g.vibratoTrust * trustFrom (stats.vibratoNotes, 3.0f);
            p.vibratoRateHz  = mix (base.vibratoRateHz, g.values.vibratoRateHz, rateTrust);
            p.vibratoOnsetMs = mix (base.vibratoOnsetMs, g.values.vibratoOnsetMs, rateTrust);
        }
        p.scoopCents = mix (base.scoopCents, g.values.scoopCents, g.scoopTrust);
        p.scoopMs    = p.scoopCents >= 1.0f ? mix (base.scoopMs > 0.0f ? base.scoopMs : g.values.scoopMs,
                                                   g.values.scoopMs > 0.0f ? g.values.scoopMs : base.scoopMs, g.scoopTrust)
                                            : 0.0f;
        p.slideMs  = mix (base.slideMs, g.values.slideMs, g.slideTrust);
        p.quantize = mix (base.quantize, g.values.quantize, g.timingTrust);
        p.swing    = mix (base.swing, g.values.swing, g.timingTrust);
        p.grit     = mix (base.grit, g.values.grit, g.toneTrust);
        p.breath   = mix (base.breath, g.values.breath, g.toneTrust);
        return p;
    }

    StyleProfile learnContrast (const StyleProfile& base, const TakeStats& genre, int genreTakes,
                                const TakeStats& pool, int poolTakes, float gain)
    {
        const auto g = suggest (genre, genreTakes);
        const auto p = suggest (pool, poolTakes);
        auto move = [gain] (float from, float gv, float pv, float trust) { return from + gain * trust * (gv - pv); };

        StyleProfile out = base;
        out.correction        = juce::jlimit (minCorrection, 1.0f, move (base.correction, g.values.correction, p.values.correction, g.pitchTrust));
        out.retuneMs          = juce::jlimit (0.0f, 150.0f, move (base.retuneMs, g.values.retuneMs, p.values.retuneMs, g.pitchTrust));
        out.keepVariation     = juce::jlimit (0.0f, 1.0f, move (base.keepVariation, g.values.keepVariation, p.values.keepVariation, g.pitchTrust));
        out.vibratoDepthCents = juce::jlimit (0.0f, 80.0f, move (base.vibratoDepthCents, g.values.vibratoDepthCents, p.values.vibratoDepthCents, g.vibratoTrust));
        if (genre.vibratoNotes > 0 && pool.vibratoNotes > 0)
        {
            const float t = g.vibratoTrust * trustFrom (genre.vibratoNotes, 3.0f);
            out.vibratoRateHz  = juce::jlimit (3.5f, 8.0f, move (base.vibratoRateHz, g.values.vibratoRateHz, p.values.vibratoRateHz, t));
            out.vibratoOnsetMs = juce::jlimit (80.0f, 900.0f, move (base.vibratoOnsetMs, g.values.vibratoOnsetMs, p.values.vibratoOnsetMs, t));
        }
        out.scoopCents = juce::jlimit (0.0f, 150.0f, move (base.scoopCents, g.values.scoopCents, p.values.scoopCents, g.scoopTrust));
        if (out.scoopCents >= 1.0f)
            out.scoopMs = juce::jlimit (20.0f, 200.0f, base.scoopMs > 0.0f ? move (base.scoopMs, g.values.scoopMs, p.values.scoopMs, g.scoopTrust)
                                                                            : juce::jmax (g.values.scoopMs, 40.0f));
        else
            out.scoopMs = 0.0f;
        out.slideMs  = juce::jlimit (0.0f, 250.0f, move (base.slideMs, g.values.slideMs, p.values.slideMs, g.slideTrust));
        if (genre.timingValid && pool.timingValid)
        {
            out.quantize = juce::jlimit (0.0f, 1.0f, move (base.quantize, g.values.quantize, p.values.quantize, g.timingTrust));
            out.swing    = juce::jlimit (0.0f, 1.0f, move (base.swing, g.values.swing, p.values.swing, g.timingTrust));
        }
        if (genre.techniqueSeconds > 0.0f && pool.techniqueSeconds > 0.0f)
        {
            out.grit   = juce::jlimit (0.0f, 1.0f, move (base.grit, g.values.grit, p.values.grit, g.toneTrust));
            out.breath = juce::jlimit (0.0f, 1.0f, move (base.breath, g.values.breath, p.values.breath, g.toneTrust));
        }
        return out;
    }

    juce::String toJson (const StyleProfile& p, const juce::String& inherits, const TakeStats& s, int takeCount)
    {
        auto v = p.toVar();
        auto* o = v.getDynamicObject();
        if (inherits.isNotEmpty() && inherits != p.id) o->setProperty ("inherits", inherits);
        if (p.description.isNotEmpty()) o->setProperty ("description", p.description);

        // Tidy numbers for people reading or editing the file.
        for (auto section : { "pitch", "vibrato", "slides", "timing", "ornaments", "tone", "dynamics" })
            if (auto* sec = o->getProperty (section).getDynamicObject())
                for (auto& prop : sec->getProperties())
                    if (prop.value.isDouble())
                        sec->setProperty (prop.name, round2 ((double) prop.value));

        o->setProperty ("learned", obj ({ { "takes", takeCount }, { "notes", s.notes },
                                          { "voicedMinutes", round2 (s.voicedSeconds / 60.0) } }));
        return juce::JSON::toString (v, false);
    }
}
