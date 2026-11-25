#include "SidechainAnalysis.h"
#include <juce_dsp/juce_dsp.h>
#include <cmath>
#include <numeric>

namespace ve::Analysis
{
    namespace
    {
        constexpr float minFreq = 65.0f;    // C2
        constexpr float maxFreq = 5000.0f;
        constexpr float silenceEnergy = 1.0e-6f;

        int fftOrderFor (double sr) { return sr > 60000.0 ? 14 : 13; } // ~5-6 Hz bins

        // Templates as intervals from the root.
        struct Template { ChordQuality q; std::vector<int> intervals; };
        const std::vector<Template>& templates()
        {
            static const std::vector<Template> t {
                { ChordQuality::major,      { 0, 4, 7 } },
                { ChordQuality::minor,      { 0, 3, 7 } },
                { ChordQuality::dominant7,  { 0, 4, 7, 10 } },
                { ChordQuality::sus2,       { 0, 2, 7 } },
                { ChordQuality::sus4,       { 0, 5, 7 } },
                { ChordQuality::diminished, { 0, 3, 6 } },
            };
            return t;
        }

        void normalise (Chroma& c)
        {
            float sum = 0.0f;
            for (auto v : c) sum += v * v;
            if (sum <= 0.0f) return;
            const float inv = 1.0f / std::sqrt (sum);
            for (auto& v : c) v *= inv;
        }

        float cosineToTemplate (const Chroma& c, int root, const std::vector<int>& intervals)
        {
            float dot = 0.0f;
            for (auto i : intervals) dot += c[(size_t) ((root + i) % 12)];
            return dot / std::sqrt ((float) intervals.size()); // c is unit length
        }

        float pearson (const Chroma& a, const std::array<float, 12>& b)
        {
            const float ma = std::accumulate (a.begin(), a.end(), 0.0f) / 12.0f;
            const float mb = std::accumulate (b.begin(), b.end(), 0.0f) / 12.0f;
            float num = 0, da = 0, db = 0;
            for (size_t i = 0; i < 12; ++i)
            {
                num += (a[i] - ma) * (b[i] - mb);
                da += (a[i] - ma) * (a[i] - ma);
                db += (b[i] - mb) * (b[i] - mb);
            }
            return (da > 0 && db > 0) ? num / std::sqrt (da * db) : 0.0f;
        }

        class ChromaEngine
        {
        public:
            explicit ChromaEngine (double sr)
                : order (fftOrderFor (sr)), size (1 << order), fft (order),
                  window ((size_t) size, juce::dsp::WindowingFunction<float>::hann, false),
                  work ((size_t) size * 2, 0.0f), binToPc ((size_t) size / 2, -1)
            {
                for (int k = 1; k < size / 2; ++k)
                {
                    const float f = (float) k * (float) sr / (float) size;
                    if (f < minFreq || f > maxFreq) continue;
                    const float midi = 69.0f + 12.0f * std::log2 (f / 440.0f);
                    binToPc[(size_t) k] = ((int) std::lround (midi) % 12 + 12) % 12;
                }
            }

            int fftSize() const noexcept { return size; }

            // Adds one frame starting at `data` (zero padded if shorter).
            void addFrame (const float* data, int available, Chroma& acc)
            {
                std::fill (work.begin(), work.end(), 0.0f);
                std::copy (data, data + std::min (available, size), work.begin());
                window.multiplyWithWindowingTable (work.data(), (size_t) size);
                fft.performFrequencyOnlyForwardTransform (work.data(), true);
                for (int k = 1; k < size / 2; ++k)
                    if (const int pc = binToPc[(size_t) k]; pc >= 0)
                        acc[(size_t) pc] += work[(size_t) k];
            }

        private:
            int order, size;
            juce::dsp::FFT fft;
            juce::dsp::WindowingFunction<float> window;
            std::vector<float> work;
            std::vector<int> binToPc;
        };

        // Sums FFT frames over [start, end) of mono.
        Chroma chromaOfRange (ChromaEngine& eng, const float* mono, int total, int start, int end)
        {
            Chroma c {};
            const int n = eng.fftSize();
            const int hop = n / 2;
            const int len = end - start;
            if (len <= 0) return c;

            if (len < n)
            {
                // One frame centred on the segment, clamped to the signal.
                int s = juce::jlimit (0, juce::jmax (0, total - n), start + len / 2 - n / 2);
                eng.addFrame (mono + s, total - s, c);
                return c;
            }
            for (int s = start; s + n <= end; s += hop)
                eng.addFrame (mono + s, n, c);
            return c;
        }
    }

    //==========================================================================
    Chroma computeChroma (const float* mono, int numSamples, double sr)
    {
        ChromaEngine eng (sr);
        return chromaOfRange (eng, mono, numSamples, 0, numSamples);
    }

    KeyEstimate estimateKey (const Chroma& chroma)
    {
        static constexpr std::array<float, 12> major { 6.35f, 2.23f, 3.48f, 2.33f, 4.38f, 4.09f, 2.52f, 5.19f, 2.39f, 3.66f, 2.29f, 2.88f };
        static constexpr std::array<float, 12> minor { 6.33f, 2.68f, 3.52f, 5.38f, 2.60f, 3.53f, 2.54f, 4.75f, 3.98f, 2.69f, 3.34f, 3.17f };

        KeyEstimate k;
        if (std::accumulate (chroma.begin(), chroma.end(), 0.0f) <= 0.0f)
            return k;

        float best = -2.0f, second = -2.0f;
        for (int mode = 0; mode < 2; ++mode)
        {
            const auto& prof = mode == 0 ? major : minor;
            for (int tonic = 0; tonic < 12; ++tonic)
            {
                std::array<float, 12> rotated {};
                for (int i = 0; i < 12; ++i)
                    rotated[(size_t) ((i + tonic) % 12)] = prof[(size_t) i];
                const float r = pearson (chroma, rotated);
                if (r > best)
                {
                    second = best;
                    best = r;
                    k.tonic = tonic;
                    k.minor = mode == 1;
                }
                else if (r > second)
                {
                    second = r;
                }
            }
        }
        k.confidence = best;
        k.margin = best - second;
        return k;
    }

    int numChordStates() { return 1 + 12 * (int) templates().size(); }

    void stateToChord (int state, int& root, ChordQuality& quality)
    {
        if (state <= 0) { root = -1; quality = ChordQuality::none; return; }
        const int t = (state - 1) / 12;
        root = (state - 1) % 12;
        quality = templates()[(size_t) t].q;
    }

    ChordScore bestChord (const Chroma& chroma)
    {
        Chroma c = chroma;
        normalise (c);
        ChordScore best { -1, ChordQuality::none, 0.0f };
        for (auto& t : templates())
            for (int root = 0; root < 12; ++root)
                if (const float s = cosineToTemplate (c, root, t.intervals); s > best.score)
                    best = { root, t.q, s };
        return best;
    }

    std::vector<int> smoothChords (const std::vector<Chroma>& beats, const std::vector<float>& energy, float selfTransition)
    {
        const int numStates = numChordStates();
        const int numBeats = (int) beats.size();
        if (numBeats == 0) return {};

        const float logStay = std::log (selfTransition);
        const float logMove = std::log ((1.0f - selfTransition) / (float) (numStates - 1));
        constexpr float sharpness = 12.0f; // softmax temperature on cosine scores

        std::vector<float> emit ((size_t) numStates);
        auto computeEmissions = [&] (int b)
        {
            const bool silent = energy[(size_t) b] < silenceEnergy;
            emit[0] = silent ? 0.0f : sharpness * 0.55f; // "no chord" baseline
            int s = 1;
            for (auto& t : templates())
                for (int root = 0; root < 12; ++root, ++s)
                {
                    // Extra notes in 7th chords make them win too easily; small penalty.
                    const float penalty = t.intervals.size() > 3 ? 0.03f : 0.0f;
                    emit[(size_t) s] = silent ? -1.0e3f : sharpness * (cosineToTemplate (beats[(size_t) b], root, t.intervals) - penalty);
                }
            float mx = *std::max_element (emit.begin(), emit.end());
            float z = 0.0f;
            for (auto v : emit) z += std::exp (v - mx);
            const float logZ = mx + std::log (z);
            for (auto& v : emit) v -= logZ;
        };

        std::vector<float> score ((size_t) numStates), next ((size_t) numStates);
        std::vector<int> back ((size_t) (numStates * numBeats));

        computeEmissions (0);
        score = emit;

        for (int b = 1; b < numBeats; ++b)
        {
            computeEmissions (b);
            int bestPrev = (int) (std::max_element (score.begin(), score.end()) - score.begin());
            const float bestPrevScore = score[(size_t) bestPrev];
            for (int s = 0; s < numStates; ++s)
            {
                const float stay = score[(size_t) s] + logStay;
                const float move = bestPrevScore + logMove;
                if (stay >= move) { next[(size_t) s] = stay + emit[(size_t) s]; back[(size_t) (b * numStates + s)] = s; }
                else              { next[(size_t) s] = move + emit[(size_t) s]; back[(size_t) (b * numStates + s)] = bestPrev; }
            }
            std::swap (score, next);
        }

        std::vector<int> path ((size_t) numBeats);
        path.back() = (int) (std::max_element (score.begin(), score.end()) - score.begin());
        for (int b = numBeats - 1; b > 0; --b)
            path[(size_t) (b - 1)] = back[(size_t) (b * numStates + path[(size_t) b])];
        return path;
    }

    juce::Result analyseSidechain (const juce::AudioBuffer<float>& audio, double sr, const TempoMap& map,
                                   JobContext* ctx, SidechainAnalysisResult& out)
    {
        out = {};
        const int total = audio.getNumSamples();
        if (total <= 0 || sr <= 0.0)
            return juce::Result::fail ("No sidechain audio");

        // Mono mix.
        std::vector<float> mono ((size_t) total, 0.0f);
        const float g = 1.0f / (float) juce::jmax (1, audio.getNumChannels());
        for (int ch = 0; ch < audio.getNumChannels(); ++ch)
            juce::FloatVectorOperations::addWithMultiply (mono.data(), audio.getReadPointer (ch), g, total);

        // Beat grid from the region's tempo map (beat length follows the time
        // signature denominator).
        TempoMap tm = map;
        if (tm.isEmpty()) tm.addPoint ({});
        tm.setSampleRate (sr);
        const double ppqStart = tm.sampleToPpq (0.0);
        const double ppqEnd = tm.sampleToPpq ((double) total);

        std::vector<std::pair<double, double>> beats;
        {
            double q = ppqStart;
            while (q < ppqEnd - 1.0e-9)
            {
                const auto& tp = tm.pointAtSample (tm.ppqToSample (q));
                const double beatLen = 4.0 / (double) juce::jmax (1, tp.denominator);
                const double next = std::min (ppqEnd, (std::floor (q / beatLen + 1.0e-9) + 1.0) * beatLen);
                if (next - q >= 0.25 * beatLen)
                    beats.emplace_back (q, next);
                else if (! beats.empty())
                    beats.back().second = next; // fold tiny leftovers into the previous beat
                q = next;
            }
        }
        if (beats.empty())
            beats.emplace_back (ppqStart, ppqEnd);

        ChromaEngine eng (sr);
        for (size_t i = 0; i < beats.size(); ++i)
        {
            if (ctx != nullptr && ctx->shouldCancel())
                return juce::Result::fail ("cancelled");

            const int s0 = juce::jlimit (0, total, (int) std::llround (tm.ppqToSample (beats[i].first)));
            const int s1 = juce::jlimit (0, total, (int) std::llround (tm.ppqToSample (beats[i].second)));

            auto c = chromaOfRange (eng, mono.data(), total, s0, s1);
            float e = 0.0f;
            for (int s = s0; s < s1; ++s) e += mono[(size_t) s] * mono[(size_t) s];
            e /= (float) juce::jmax (1, s1 - s0);

            for (size_t p = 0; p < 12; ++p)
                out.chromaSum[p] += c[p];
            normalise (c);

            out.beatStartPpq.push_back (beats[i].first);
            out.beatChroma.push_back (c);
            out.beatEnergy.push_back (e);

            if (ctx != nullptr)
                ctx->setProgress (0.9f * (float) (i + 1) / (float) beats.size());
        }

        out.key = estimateKey (out.chromaSum);

        const auto path = smoothChords (out.beatChroma, out.beatEnergy);
        for (size_t i = 0; i < path.size(); ++i)
        {
            int root; ChordQuality q;
            stateToChord (path[i], root, q);
            float conf = 0.0f;
            if (root >= 0)
                for (auto& t : templates())
                    if (t.q == q) conf = cosineToTemplate (out.beatChroma[i], root, t.intervals);

            const double end = beats[i].second;
            if (! out.chords.empty() && out.chords.back().root == root && out.chords.back().quality == q)
            {
                auto& last = out.chords.back();
                const double n0 = last.endPpq - last.startPpq, n1 = end - beats[i].first;
                last.confidence = (float) ((last.confidence * n0 + conf * n1) / juce::jmax (1.0e-9, n0 + n1));
                last.endPpq = end;
            }
            else
            {
                out.chords.push_back ({ beats[i].first, end, root, q, conf });
            }
        }

        if (ctx != nullptr) ctx->setProgress (1.0f);
        return juce::Result::ok();
    }

    //==========================================================================
    juce::String pitchClassName (int pc)
    {
        static const char* names[] = { "C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" };
        return pc >= 0 && pc < 12 ? juce::String (names[pc]) : juce::String ("-");
    }

    juce::String keyName (int tonic, bool minor)
    {
        if (tonic < 0) return "Unknown";
        return pitchClassName (tonic) + (minor ? " minor" : " major");
    }

    juce::String chordName (int root, ChordQuality q)
    {
        if (root < 0 || q == ChordQuality::none) return "N.C.";
        const auto r = pitchClassName (root);
        switch (q)
        {
            case ChordQuality::major:      return r;
            case ChordQuality::minor:      return r + "m";
            case ChordQuality::dominant7:  return r + "7";
            case ChordQuality::sus2:       return r + "sus2";
            case ChordQuality::sus4:       return r + "sus4";
            case ChordQuality::diminished: return r + "dim";
            case ChordQuality::none:       break;
        }
        return "N.C.";
    }
}
