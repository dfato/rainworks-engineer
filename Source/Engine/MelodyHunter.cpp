#include "MelodyHunter.h"
#include "NotePlan.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <numeric>
#include <random>

namespace ve::MelodyHunt
{
    namespace
    {
        // ---- genre flavours ------------------------------------------------------
        struct Flavouring
        {
            std::array<float, 6> stepWeight;   // chance of moving 0, 1, 2, 3, 4, 5+ scale steps
            float motifReturn;                 // chance a later phrase brings back an earlier one
            float chordPull;                   // how hard strong beats want chord tones
            float rangeScale;                  // multiplies the range span
            int climaxLift;                    // scale steps the peak phrase sits higher
            float pentatonicBias;              // leaning to the five "safe" notes of the key
            float repeatedNotes;               // tolerance for three or more of the same note
            std::array<Contour, 4> contours;   // what Automatic picks from
            float registerSemis;               // nudges where the melody sits
        };

        const Flavouring& flavouring (Flavour f)
        {
            using C = Contour;
            static const Flavouring pop    { { 0.18f, 0.42f, 0.22f, 0.10f, 0.05f, 0.03f }, 0.55f, 1.0f, 1.00f, 2, 0.25f, 0.35f, { C::arch, C::wave, C::rise, C::arch }, 0.0f };
            // Scene / emo-pop: soaring lines, a big lift into the peak phrase, bolder leaps.
            static const Flavouring rock   { { 0.15f, 0.34f, 0.22f, 0.13f, 0.10f, 0.06f }, 0.50f, 1.1f, 1.25f, 3, 0.20f, 0.35f, { C::rise, C::arch, C::arch, C::fall }, 1.0f };
            // Smooth stepwise lines; colour tones (2nds, 6ths) are welcome.
            static const Flavouring rnb    { { 0.10f, 0.50f, 0.22f, 0.10f, 0.05f, 0.03f }, 0.40f, 0.7f, 1.10f, 2, 0.00f, 0.25f, { C::wave, C::fall, C::arch, C::wave }, 0.0f };
            // Chant-like hooks: narrow range, lots of repeated notes and returning motifs.
            static const Flavouring hipHop { { 0.42f, 0.34f, 0.14f, 0.06f, 0.03f, 0.01f }, 0.65f, 0.9f, 0.70f, 1, 0.45f, 0.95f, { C::hook, C::wave, C::fall, C::hook }, 0.0f };
            static const Flavouring trap   { { 0.38f, 0.36f, 0.15f, 0.07f, 0.03f, 0.01f }, 0.70f, 0.9f, 0.75f, 1, 0.60f, 0.85f, { C::hook, C::fall, C::wave, C::valley }, -1.0f };
            // Wider, less predictable intervals and contours.
            static const Flavouring indie  { { 0.15f, 0.30f, 0.20f, 0.15f, 0.11f, 0.09f }, 0.40f, 0.8f, 1.10f, 2, 0.10f, 0.45f, { C::valley, C::rise, C::wave, C::fall }, 0.0f };
            switch (f)
            {
                case Flavour::rock:   return rock;
                case Flavour::rnb:    return rnb;
                case Flavour::hipHop: return hipHop;
                case Flavour::trap:   return trap;
                case Flavour::indie:  return indie;
                default:              return pop;
            }
        }

        float contourShape (Contour c, float t) noexcept   // -1 .. 1 over the phrase
        {
            const float pi = juce::MathConstants<float>::pi;
            switch (c)
            {
                case Contour::arch:   return -0.6f + 1.6f * std::sin (pi * t);
                case Contour::valley: return 0.6f - 1.6f * std::sin (pi * t);
                case Contour::rise:   return -0.9f + 1.8f * t;
                case Contour::fall:   return 0.9f - 1.8f * t;
                case Contour::wave:   return 0.8f * std::sin (2.0f * pi * t);
                case Contour::hook:   return 0.25f * std::sin (2.0f * pi * t);
                default:              return 0.0f;
            }
        }

        // ---- harmony -------------------------------------------------------------
        using PcSet = std::array<bool, 12>;

        PcSet chordPcs (int root, ChordQuality q)
        {
            PcSet s {};
            if (root < 0) return s;
            std::vector<int> iv;
            switch (q)
            {
                case ChordQuality::major:      iv = { 0, 4, 7 }; break;
                case ChordQuality::minor:      iv = { 0, 3, 7 }; break;
                case ChordQuality::dominant7:  iv = { 0, 4, 7, 10 }; break;
                case ChordQuality::sus2:       iv = { 0, 2, 7 }; break;
                case ChordQuality::sus4:       iv = { 0, 5, 7 }; break;
                case ChordQuality::diminished: iv = { 0, 3, 6 }; break;
                default: return s;
            }
            for (int i : iv) s[(size_t) ((root + i) % 12)] = true;
            return s;
        }

        struct ImpliedChord { int degreeSemis; ChordQuality q; };

        // Loops that most songs in these genres sit on, used when there is no beat to read.
        std::vector<ImpliedChord> impliedProgression (bool minor, Flavour f, std::mt19937& rng)
        {
            using Q = ChordQuality;
            static const std::vector<std::vector<ImpliedChord>> major {
                { { 0, Q::major }, { 7, Q::major }, { 9, Q::minor }, { 5, Q::major } },   // I V vi IV
                { { 9, Q::minor }, { 5, Q::major }, { 0, Q::major }, { 7, Q::major } },   // vi IV I V
                { { 0, Q::major }, { 9, Q::minor }, { 5, Q::major }, { 7, Q::major } },   // I vi IV V
                { { 0, Q::major }, { 5, Q::major }, { 9, Q::minor }, { 7, Q::major } },   // I IV vi V
            };
            static const std::vector<std::vector<ImpliedChord>> minorProg {
                { { 0, Q::minor }, { 8, Q::major }, { 3, Q::major }, { 10, Q::major } },  // i VI III VII
                { { 0, Q::minor }, { 10, Q::major }, { 8, Q::major }, { 10, Q::major } }, // i VII VI VII
                { { 0, Q::minor }, { 5, Q::minor }, { 8, Q::major }, { 7, Q::minor } },   // i iv VI v
                { { 0, Q::minor }, { 8, Q::major }, { 0, Q::minor }, { 8, Q::major } },   // i VI i VI
            };
            const auto& set = minor ? minorProg : major;
            // Trap and hip-hop beats tend to sit on two-chord loops.
            if (minor && (f == Flavour::trap || f == Flavour::hipHop) && std::uniform_real_distribution<float> (0, 1) (rng) < 0.5f)
                return set[3];
            return set[(size_t) std::uniform_int_distribution<int> (0, (int) set.size() - 1) (rng)];
        }

        float beatStrength (double ppq, double beatsPerBar) noexcept
        {
            const double bar = std::fmod (std::fmod (ppq, beatsPerBar) + beatsPerBar, beatsPerBar);
            auto near = [] (double x, double grid) { const double r = std::fmod (x, grid); return juce::jmin (r, grid - r) < 0.13; };
            if (bar < 0.13 || bar > beatsPerBar - 0.13) return 1.0f;
            if (std::abs (bar - beatsPerBar * 0.5) < 0.13) return 0.75f;
            if (near (bar, 1.0)) return 0.5f;
            if (near (bar, 0.5)) return 0.3f;
            return 0.15f;
        }

        int pcOf (int midi) noexcept { return ((midi % 12) + 12) % 12; }

        // ---- per-note context ----------------------------------------------------
        struct Slot
        {
            double start = 0.0, length = 0.0;
            float sung = 60.0f, current = 60.0f;
            float strength = 0.15f;
            PcSet chord {};
            bool hasChord = false;
        };

        struct Phrase
        {
            int begin = 0, end = 0;          // slots [begin, end)
            int label = 0;                   // A = 0, B = 1 ...
            int source = -1;                 // phrase it repeats (-1 = new material)
            Contour contour = Contour::arch;
            float lift = 0.0f;               // scale steps
            std::array<bool, 12> cadence {}; // pitch classes the phrase wants to end on
            bool final = false;
        };

        struct Context
        {
            std::vector<int> pitches;        // in-scale MIDI notes inside the range, low to high
            std::vector<Slot> slots;
            const Flavouring* fl = nullptr;
            Options opt;
            KeyEstimate key;
            PcSet stable {}, pentatonic {};
            float centre = 0.0f, amp = 0.0f;
            float hookiness = 1.0f;
        };

        int nearestIndex (const std::vector<int>& p, float midi)
        {
            int best = 0;
            for (int i = 1; i < (int) p.size(); ++i)
                if (std::abs ((float) p[(size_t) i] - midi) < std::abs ((float) p[(size_t) best] - midi)) best = i;
            return best;
        }

        float targetDegree (const Context& c, const Phrase& ph, int k)
        {
            const int len = ph.end - ph.begin;
            const float t = len <= 1 ? 0.5f : (float) k / (float) (len - 1);
            return c.centre + ph.lift + c.amp * contourShape (ph.contour, t);
        }

        // ---- candidate generation ------------------------------------------------
        template <typename Rng>
        int pick (const std::vector<float>& w, Rng& rng)
        {
            const float total = std::accumulate (w.begin(), w.end(), 0.0f);
            if (total <= 0.0f) return (int) (std::uniform_int_distribution<size_t> (0, w.size() - 1) (rng));
            float r = std::uniform_real_distribution<float> (0.0f, total) (rng);
            for (size_t i = 0; i < w.size(); ++i)
                if ((r -= w[i]) <= 0.0f) return (int) i;
            return (int) w.size() - 1;
        }

        // One note of a random walk: weights over every pitch in range.
        int walkStep (const Context& c, const Phrase& ph, int k, const std::vector<int>& line, int before, std::mt19937& rng)
        {
            const auto& s = c.slots[(size_t) (ph.begin + k)];
            const int n = (int) c.pitches.size();
            const int len = ph.end - ph.begin;
            const int prev = k > 0 ? line[(size_t) k - 1] : before;
            const int prev2 = k > 1 ? line[(size_t) k - 2] : (k == 1 ? before : -1);
            const float tgt = targetDegree (c, ph, k);
            std::vector<float> w ((size_t) n, 0.0f);
            for (int d = 0; d < n; ++d)
            {
                const int pc = pcOf (c.pitches[(size_t) d]);
                float x = std::exp (-0.35f * std::abs ((float) d - tgt));
                if (prev >= 0)
                {
                    const int iv = std::abs (d - prev);
                    x *= c.fl->stepWeight[(size_t) juce::jmin (iv, 5)] * (iv > 5 ? std::pow (0.6f, (float) (iv - 5)) : 1.0f);
                    if (k == 0) x = std::sqrt (x);   // a new phrase may jump a little more
                    // After a leap, turn around (gap fill).
                    if (prev2 >= 0 && std::abs (prev - prev2) >= 3 && (d - prev) * (prev - prev2) > 0) x *= 0.3f;
                    if (prev2 >= 0 && d == prev && prev == prev2) x *= c.fl->repeatedNotes;
                }
                if (s.hasChord && s.chord[(size_t) pc])  x *= 1.0f + c.fl->chordPull * s.strength * 2.0f;
                else if (s.strength >= 0.75f)            x *= std::pow (0.35f, c.fl->chordPull);
                if (! c.pentatonic[(size_t) pc])         x *= 1.0f - 0.6f * c.fl->pentatonicBias;
                if (k == len - 1 && ph.cadence[(size_t) pc]) x *= 4.0f;
                w[(size_t) d] = x;
            }
            return pick (w, rng);
        }

        std::vector<int> freshLine (const Context& c, const Phrase& ph, int before, std::mt19937& rng)
        {
            std::vector<int> line;
            for (int k = 0; k < ph.end - ph.begin; ++k)
                line.push_back (walkStep (c, ph, k, line, before, rng));
            return line;
        }

        // Brings back an earlier phrase: same shape mapped onto this rhythm, maybe moved
        // up/down the scale (a sequence) and with a new tail.
        std::vector<int> motifLine (const Context& c, const Phrase& ph, const std::vector<int>& src, int before, std::mt19937& rng)
        {
            const int len = ph.end - ph.begin, ls = (int) src.size(), n = (int) c.pitches.size();
            std::uniform_real_distribution<float> u (0.0f, 1.0f);
            int shift = 0;
            const float r = u (rng);
            if (r > 0.6f) shift = r > 0.8f ? (u (rng) < 0.5f ? -2 : 2) : (u (rng) < 0.5f ? -1 : 1);
            std::vector<int> line;
            for (int k = 0; k < len; ++k)
            {
                const int j = ls <= 1 || len <= 1 ? 0 : juce::roundToInt ((float) k * (float) (ls - 1) / (float) (len - 1));
                line.push_back (juce::jlimit (0, n - 1, src[(size_t) juce::jmin (j, ls - 1)] + shift));
            }
            // New ending (answer phrase) on most returns; keeps the hook but resolves differently.
            const int tail = u (rng) < 0.7f ? juce::jmin (len - 1, 1 + (int) (u (rng) * 2.0f)) : 0;
            for (int k = len - tail; k < len; ++k)
            {
                std::vector<int> head (line.begin(), line.begin() + k);
                line[(size_t) k] = walkStep (c, ph, k, head, before, rng);
            }
            return line;
        }

        // Starts from the notes as they are now and moves a few of them.
        std::vector<int> nudgedLine (const Context& c, const Phrase& ph, std::mt19937& rng)
        {
            const int len = ph.end - ph.begin, n = (int) c.pitches.size();
            std::uniform_real_distribution<float> u (0.0f, 1.0f);
            std::vector<int> line;
            for (int k = 0; k < len; ++k)
                line.push_back (nearestIndex (c.pitches, c.slots[(size_t) (ph.begin + k)].current));
            for (int k = 0; k < len; ++k)
            {
                if (u (rng) > 0.3f) continue;
                const auto& s = c.slots[(size_t) (ph.begin + k)];
                int d = line[(size_t) k];
                if (s.hasChord && s.strength >= 0.5f && u (rng) < 0.6f)
                {
                    // nearest chord tone other than the current note
                    for (int off = 1; off < n; ++off)
                    {
                        const int dir = u (rng) < 0.5f ? 1 : -1;
                        const int a = d + dir * off, b = d - dir * off;
                        if (a >= 0 && a < n && s.chord[(size_t) pcOf (c.pitches[(size_t) a])]) { d = a; break; }
                        if (b >= 0 && b < n && s.chord[(size_t) pcOf (c.pitches[(size_t) b])]) { d = b; break; }
                    }
                }
                else
                    d += (u (rng) < 0.5f ? -1 : 1) * (u (rng) < 0.75f ? 1 : 2);
                line[(size_t) k] = juce::jlimit (0, n - 1, d);
            }
            return line;
        }

        // ---- scoring -------------------------------------------------------------
        float similarity (const std::vector<int>& a, const std::vector<int>& b)
        {
            // Share of matching interval moves (direction and size), mapped by position.
            const int la = (int) a.size(), lb = (int) b.size();
            if (la < 2 || lb < 2) return 0.0f;
            int same = 0;
            for (int k = 1; k < la; ++k)
            {
                const int j = juce::jlimit (1, lb - 1, juce::roundToInt ((float) k * (float) (lb - 1) / (float) (la - 1)));
                if (a[(size_t) k] - a[(size_t) k - 1] == b[(size_t) j] - b[(size_t) j - 1]) ++same;
            }
            return (float) same / (float) (la - 1);
        }

        float scoreLine (const Context& c, const Phrase& ph, const std::vector<int>& line, int before,
                         const std::vector<int>* source, const std::vector<int>* firstPhrase)
        {
            const int len = (int) line.size();
            const auto& fl = *c.fl;
            float score = 0.0f;
            int steps = 0, moves = 0, top = -1, topCount = 0, unique = 0;
            std::array<bool, 128> seen {};
            for (int k = 0; k < len; ++k)
            {
                const auto& s = c.slots[(size_t) (ph.begin + k)];
                const int m = c.pitches[(size_t) line[(size_t) k]], pc = pcOf (m);
                const float weight = s.strength * 0.6f + (float) juce::jmin (2.0, s.length) * 0.4f;
                const bool chordTone = s.hasChord && s.chord[(size_t) pc];
                if (chordTone)                score += weight * fl.chordPull;
                else if (c.stable[(size_t) pc]) score += 0.2f * weight;
                else                          score -= 0.7f * weight * fl.chordPull;

                score -= 0.12f * std::abs ((float) line[(size_t) k] - targetDegree (c, ph, k));
                // Big shifts from what was sung sound processed: keep most notes within a fifth.
                score -= 0.15f * juce::jmax (0.0f, std::abs ((float) m - s.sung) - 7.0f);

                if (! seen[(size_t) juce::jlimit (0, 127, m)]) { seen[(size_t) juce::jlimit (0, 127, m)] = true; ++unique; }
                if (m > top) { top = m; topCount = 1; } else if (m == top) ++topCount;

                const int prevMidi = k > 0 ? c.pitches[(size_t) line[(size_t) k - 1]] : (before >= 0 ? c.pitches[(size_t) before] : -1);
                if (prevMidi >= 0)
                {
                    const int iv = std::abs (m - prevMidi);
                    if (k == 0) { if (iv > 9) score -= 0.8f; }
                    else
                    {
                        ++moves;
                        if (std::abs (line[(size_t) k] - line[(size_t) k - 1]) == 1) ++steps;   // one scale step
                        if (iv == 6) score -= 0.8f;                  // tritone leap
                        if (iv > 9)  score -= 0.6f + 0.25f * (float) (iv - 9);
                        if (iv > 12) score -= 2.0f;
                        if (k >= 2)
                        {
                            const int pm2 = c.pitches[(size_t) line[(size_t) k - 2]];
                            const int a = prevMidi - pm2, b = m - prevMidi;
                            if (std::abs (a) >= 5 && std::abs (b) >= 5 && a * b > 0) score -= 0.7f;   // two leaps the same way
                            if (std::abs (a) >= 5 && std::abs (b) <= 2 && b != 0 && a * b < 0) score += 0.3f;  // leap then step back
                        }
                    }
                    // A leading tone wants to go home.
                    if (k > 0 && pcOf (prevMidi - c.key.tonic) == 11 && pcOf (m - c.key.tonic) != 0) score -= 0.5f;
                }
            }

            // Step/leap balance close to the flavour's.
            if (moves > 0)
                score -= 2.0f * std::abs ((float) steps / (float) moves - fl.stepWeight[1]);

            // Little repeated cells (the same two moves twice) make a line stick.
            int cells = 0;
            for (int i = 2; i < len; ++i)
                for (int j = i + 1; j < len; ++j)
                    if (line[(size_t) i] - line[(size_t) i - 1] == line[(size_t) j] - line[(size_t) j - 1]
                        && line[(size_t) i - 1] - line[(size_t) i - 2] == line[(size_t) j - 1] - line[(size_t) j - 2])
                        ++cells;
            score += juce::jmin (1.0f, 0.25f * (float) cells) * c.hookiness;

            if (len >= 4 && topCount == 1) score += 0.4f;                                   // one clear high point
            if (len >= 5 && unique <= 2 && ph.contour != Contour::hook) score -= 1.0f;       // too flat

            // Cadence.
            const int last = c.pitches[(size_t) line.back()], lpc = pcOf (last);
            const auto& ls = c.slots[(size_t) (ph.end - 1)];
            if (ph.final && c.opt.endOnRoot)   score += lpc == c.key.tonic ? 2.5f : -1.0f;
            else if (ph.cadence[(size_t) lpc]) score += 1.5f;
            if (ls.hasChord && ls.chord[(size_t) lpc]) score += 0.5f;
            else if (! c.stable[(size_t) lpc])         score -= 1.0f;

            if (source != nullptr)          score += 1.5f * similarity (line, *source) * c.hookiness;
            else if (firstPhrase != nullptr) score -= 0.8f * juce::jmax (0.0f, similarity (line, *firstPhrase) - 0.5f);

            if (c.opt.variation == Variation::subtle)
            {
                float moved = 0.0f;
                for (int k = 0; k < len; ++k)
                    moved += std::abs ((float) c.pitches[(size_t) line[(size_t) k]] - c.slots[(size_t) (ph.begin + k)].current);
                score -= 0.35f * moved / (float) len;
            }
            return score;
        }

        PcSet scaleSet (const KeyEstimate& key)
        {
            PcSet s {};
            for (int pc = 0; pc < 12; ++pc)
                s[(size_t) pc] = NoteSegmenter::isInScale (60 + pc, key);
            return s;
        }
    }

    //==========================================================================
    Result hunt (const std::vector<InNote>& in, const KeyEstimate& keyIn, const std::vector<ChordSpan>& chords,
                 const Options& opt, std::uint32_t seed, double beatsPerBar)
    {
        Result res;
        res.midi.reserve (in.size());
        for (auto& n : in) res.midi.push_back (n.currentMidi);
        if (in.empty()) return res;

        std::mt19937 rng (seed);
        std::uniform_real_distribution<float> u (0.0f, 1.0f);
        beatsPerBar = beatsPerBar > 0.5 ? beatsPerBar : 4.0;

        Context c;
        c.opt = opt;
        res.flavour = opt.flavour == Flavour::followStyle ? Flavour::pop : opt.flavour;
        c.fl = &flavouring (res.flavour);

        // Key and scale: the loaded one; a key guessed from the notes without one. A
        // chromatic "scale" has no notes to choose between, so use the key's own scale.
        c.key = keyIn.isValid() ? keyIn : keyFromNotes (in);
        if (! c.key.isValid()) { c.key.tonic = 0; c.key.minor = false; }
        if (c.key.scale == ScaleMode::chromatic) c.key.scale = ScaleMode::key;
        res.key = c.key;
        const auto inScale = scaleSet (c.key);
        for (int pc : { 0, c.key.minor ? 3 : 4, 7 }) c.stable[(size_t) ((c.key.tonic + pc) % 12)] = true;
        for (int pc : c.key.minor ? std::vector<int> { 0, 3, 5, 7, 10 } : std::vector<int> { 0, 2, 4, 7, 9 })
            c.pentatonic[(size_t) ((c.key.tonic + pc) % 12)] = true;
        if (opt.variation == Variation::wild) for (auto& p : c.pentatonic) p = true;   // wild: no leaning

        // Order notes in time.
        std::vector<int> order ((size_t) in.size());
        std::iota (order.begin(), order.end(), 0);
        std::stable_sort (order.begin(), order.end(), [&] (int a, int b) { return in[(size_t) a].startPpq < in[(size_t) b].startPpq; });

        // Range: centred on what was sung so the voice is not pushed far.
        std::vector<float> sung;
        for (auto& n : in) sung.push_back (n.sungMidi);
        std::nth_element (sung.begin(), sung.begin() + (long) sung.size() / 2, sung.end());
        float centreMidi = sung[sung.size() / 2] + c.fl->registerSemis;
        if (opt.registerShift == Register::higher) centreMidi += 3.0f;
        if (opt.registerShift == Register::lower)  centreMidi -= 3.0f;
        const float baseSpan = opt.range == Range::tight ? 8.0f : opt.range == Range::wide ? 17.0f : 12.0f;
        float span = juce::jlimit (6.0f, 20.0f, baseSpan * c.fl->rangeScale);
        for (int tries = 0; tries < 4; ++tries, span += 3.0f)
        {
            c.pitches.clear();
            const int lo = (int) std::floor (centreMidi - span * 0.5f), hi = (int) std::ceil (centreMidi + span * 0.5f);
            for (int m = juce::jmax (0, lo); m <= juce::jmin (127, hi); ++m)
                if (inScale[(size_t) pcOf (m)]) c.pitches.push_back (m);
            if (c.pitches.size() >= 5) break;
        }
        if (c.pitches.empty()) return res;
        c.centre = (float) nearestIndex (c.pitches, centreMidi);
        c.amp = (float) (c.pitches.size() - 1) * 0.3f;
        c.hookiness = opt.repetition == Repetition::hooky ? 1.4f : opt.repetition == Repetition::free ? 0.4f : 1.0f;

        // Harmony per note: the beat's chords, or a common loop for the key.
        bool anyChord = false;
        if (opt.followChords)
            for (auto& ch : chords) if (ch.root >= 0 && ch.quality != ChordQuality::none) { anyChord = true; break; }
        std::vector<ImpliedChord> prog;
        if (! anyChord) prog = impliedProgression (c.key.minor, res.flavour, rng);
        const double firstBar = std::floor (in[(size_t) order.front()].startPpq / beatsPerBar);
        const int barsPerChord = (res.flavour == Flavour::trap || res.flavour == Flavour::hipHop) ? 2 : 1;
        for (int oi : order)
        {
            const auto& n = in[(size_t) oi];
            Slot s;
            s.start = n.startPpq;
            s.length = n.lengthPpq;
            s.sung = n.sungMidi;
            s.current = n.currentMidi;
            s.strength = beatStrength (n.startPpq, beatsPerBar);
            // Long notes carry weight wherever they start.
            if (n.lengthPpq >= 1.5) s.strength = juce::jmax (s.strength, 0.6f);
            if (anyChord)
            {
                const double mid = n.startPpq + juce::jmin (n.lengthPpq * 0.5, 0.25);
                for (auto& ch : chords)
                    if (mid >= ch.startPpq && mid < ch.endPpq && ch.root >= 0 && ch.quality != ChordQuality::none)
                    {
                        s.chord = chordPcs (ch.root, ch.quality);
                        s.hasChord = true;
                        break;
                    }
            }
            else
            {
                const int bar = (int) (std::floor (n.startPpq / beatsPerBar) - firstBar);
                const auto& ic = prog[(size_t) ((juce::jmax (0, bar) / barsPerChord) % (int) prog.size())];
                s.chord = chordPcs ((c.key.tonic + ic.degreeSemis) % 12, ic.q);
                s.hasChord = true;
            }
            c.slots.push_back (s);
        }
        res.usedDetectedChords = anyChord;

        // Phrases: a rest of more than about half a beat, or two bars, or 16 notes.
        std::vector<Phrase> phrases;
        {
            int b = 0;
            for (int i = 1; i <= (int) c.slots.size(); ++i)
            {
                bool split = i == (int) c.slots.size();
                if (! split)
                {
                    const auto& p = c.slots[(size_t) i - 1];
                    const auto& s = c.slots[(size_t) i];
                    const double gap = s.start - (p.start + p.length);
                    const bool newBar = std::floor (s.start / beatsPerBar) != std::floor (p.start / beatsPerBar);
                    split = gap >= 0.6 || (s.start - c.slots[(size_t) b].start >= 2.0 * beatsPerBar && newBar) || i - b >= 16;
                }
                if (split) { Phrase ph; ph.begin = b; ph.end = i; phrases.push_back (ph); b = i; }
            }
        }
        // Fold single-note scraps into the phrase before them.
        for (size_t i = 1; i < phrases.size();)
            if (phrases[i].end - phrases[i].begin == 1 && phrases[i - 1].end - phrases[i - 1].begin < 16)
            {
                phrases[i - 1].end = phrases[i].end;
                phrases.erase (phrases.begin() + (long) i);
            }
            else ++i;

        // Form: which phrases bring back earlier ones, contours, the peak, cadences.
        const int np = (int) phrases.size();
        const float returnChance = juce::jlimit (0.0f, 0.95f, c.fl->motifReturn * (opt.repetition == Repetition::hooky ? 1.35f : opt.repetition == Repetition::free ? 0.4f : 1.0f)
                                                              * (opt.variation == Variation::wild ? 0.75f : 1.0f));
        // Form in blocks of four phrases, like a verse or a chorus (AABA, ABAB ...). A later
        // block either comes back as an earlier one (verse 2) or is a new section, sat higher.
        static const std::array<const char*, 4> hookyForms { "AABA", "AAAB", "ABAB", "AAAA" };
        static const std::array<const char*, 4> balancedForms { "AABA", "ABAB", "ABAC", "AABB" };
        static const std::array<const char*, 4> freeForms { "ABCD", "ABCA", "ABAC", "ABCB" };
        const auto& forms = returnChance > 0.6f ? (u (rng) < 0.6f ? hookyForms : balancedForms)
                          : returnChance < 0.3f ? freeForms
                          : (u (rng) < 0.25f ? hookyForms : balancedForms);
        int labels = 0;
        std::vector<float> sectionLift ((size_t) np, 0.0f);
        int newSections = 0;
        for (int base = 0; base < np; base += 4)
        {
            const bool repeatSection = base > 0 && u (rng) < 0.3f + 0.3f * returnChance;
            const int ref = repeatSection ? (base >= 8 && u (rng) < 0.5f ? base - 8 : 0) : -1;
            const char* form = forms[(size_t) std::uniform_int_distribution<int> (0, 3) (rng)];
            const float lift = base > 0 && ! repeatSection ? (float) juce::jmin (2, ++newSections) * 0.75f : 0.0f;
            std::map<char, int> local;   // letter -> first phrase with it in this block
            for (int k = 0; k < 4 && base + k < np; ++k)
            {
                const int p = base + k;
                auto& ph = phrases[(size_t) p];
                if (repeatSection && ref + k < base)
                {
                    const auto& r = phrases[(size_t) (ref + k)];
                    ph.source = r.source >= 0 ? r.source : ref + k;
                    ph.label = r.label;
                    sectionLift[(size_t) p] = sectionLift[(size_t) (ref + k)];
                    continue;
                }
                const char letter = form[k];
                if (auto it = local.find (letter); it != local.end())
                {
                    ph.source = it->second;
                    ph.label = phrases[(size_t) it->second].label;
                }
                else
                {
                    local[letter] = p;
                    ph.label = labels++;
                }
                sectionLift[(size_t) p] = lift;
            }
        }
        const int climax = np >= 3 ? juce::roundToInt (0.66f * (float) (np - 1)) : -1;
        const Contour autoFirst = c.fl->contours[(size_t) std::uniform_int_distribution<int> (0, 3) (rng)];
        for (int p = 0; p < np; ++p)
        {
            auto& ph = phrases[(size_t) p];
            if (opt.contour != Contour::automatic)     ph.contour = opt.contour;
            else if (ph.source >= 0)                  ph.contour = phrases[(size_t) ph.source].contour;
            else if (p == 0)                          ph.contour = autoFirst;
            else                                      ph.contour = c.fl->contours[(size_t) std::uniform_int_distribution<int> (0, 3) (rng)];
            if (opt.contour == Contour::hook) ph.contour = Contour::hook;

            ph.lift = sectionLift[(size_t) p] + (p == climax ? (float) c.fl->climaxLift : (ph.label > 0 && ph.source < 0 ? 0.5f : 0.0f));
            ph.final = p == np - 1;
            // Question / answer: odd phrases settle on the root or third, even ones stay open.
            const auto add = [&] (int semis) { ph.cadence[(size_t) ((c.key.tonic + semis) % 12)] = true; };
            if (ph.final || p % 2 == 1) { add (0); add (c.key.minor ? 3 : 4); }
            else                        { add (7); add (2); }
        }

        // Write each phrase: generate candidates, score them, pick one with some chance.
        const float temperature = opt.variation == Variation::subtle ? 0.35f : opt.variation == Variation::wild ? 1.3f : 0.6f;
        std::vector<std::vector<int>> lines ((size_t) np);
        for (int p = 0; p < np; ++p)
        {
            const auto& ph = phrases[(size_t) p];
            const int before = p > 0 ? lines[(size_t) p - 1].back() : -1;
            const std::vector<int>* src = ph.source >= 0 ? &lines[(size_t) ph.source] : nullptr;
            const std::vector<int>* first = p > 0 && ph.source < 0 ? &lines[0] : nullptr;

            std::vector<std::vector<int>> cands;
            if (opt.variation == Variation::subtle)
            {
                cands.push_back ([&] { std::vector<int> l; for (int k = ph.begin; k < ph.end; ++k) l.push_back (nearestIndex (c.pitches, c.slots[(size_t) k].current)); return l; }());
                for (int i = 0; i < 40; ++i) cands.push_back (nudgedLine (c, ph, rng));
            }
            else
            {
                const int fresh = src != nullptr ? 14 : 48;
                for (int i = 0; i < fresh; ++i) cands.push_back (freshLine (c, ph, before, rng));
                if (src != nullptr)
                    for (int i = 0; i < 28; ++i) cands.push_back (motifLine (c, ph, *src, before, rng));
            }

            std::vector<float> scores;
            for (auto& l : cands) scores.push_back (scoreLine (c, ph, l, before, src, first));
            const float best = *std::max_element (scores.begin(), scores.end());
            std::vector<float> w;
            for (float s : scores) w.push_back (std::exp ((s - best) / temperature));
            auto line = cands[(size_t) pick (w, rng)];
            // The very last note goes home: the root nearest the note before it.
            if (ph.final && opt.endOnRoot && pcOf (c.pitches[(size_t) line.back()] - c.key.tonic) != 0)
            {
                const int from = line.size() > 1 ? line[line.size() - 2] : (before >= 0 ? before : line.back());
                int bestRoot = -1;
                for (int d = 0; d < (int) c.pitches.size(); ++d)
                    if (pcOf (c.pitches[(size_t) d] - c.key.tonic) == 0
                        && (bestRoot < 0 || std::abs (d - from) < std::abs (bestRoot - from)))
                        bestRoot = d;
                if (bestRoot >= 0) line.back() = bestRoot;
            }
            lines[(size_t) p] = std::move (line);
        }

        // Back to input order.
        juce::String form;
        for (int p = 0; p < np; ++p)
        {
            const auto& ph = phrases[(size_t) p];
            for (int k = ph.begin; k < ph.end; ++k)
                res.midi[(size_t) order[(size_t) k]] = (float) c.pitches[(size_t) lines[(size_t) p][(size_t) (k - ph.begin)]];
            if (p > 0) form << " ";
            form << juce::String::charToString ((juce::juce_wchar) ('A' + juce::jmin (ph.label, 25)));
            if (ph.source >= 0 && similarity (lines[(size_t) p], lines[(size_t) ph.source]) < 0.99f) form << "'";
        }
        res.phrases = np;
        res.form = form;
        res.description << "Hunted " << (int) in.size() << (in.size() == 1 ? " note" : " notes") << " in " << np
                        << (np == 1 ? " phrase" : " phrases") << " (" << form << "): " << toString (res.flavour)
                        << ", " << Analysis::keyName (c.key.tonic, c.key.minor)
                        << (c.key.scale != ScaleMode::key ? " " + ve::toString (c.key.scale) : juce::String())
                        << (anyChord ? ", on the beat's chords" : ", on a " + toString (res.flavour) + " chord loop");
        return res;
    }

    Flavour flavourForStyle (const juce::String& id) noexcept
    {
        if (id == "rock")    return Flavour::rock;
        if (id == "rnb")     return Flavour::rnb;
        if (id == "hip-hop") return Flavour::hipHop;
        if (id == "trap")    return Flavour::trap;
        if (id == "indie")   return Flavour::indie;
        return Flavour::pop;
    }

    Flavour resolve (Flavour f, const juce::String& styleId) noexcept
    {
        return f == Flavour::followStyle ? flavourForStyle (styleId) : f;
    }

    KeyEstimate keyFromNotes (const std::vector<InNote>& notes)
    {
        Chroma ch {};
        for (auto& n : notes)
            ch[(size_t) pcOf ((int) std::lround (n.sungMidi))] += (float) juce::jmax (0.05, n.lengthPpq);
        float total = 0.0f;
        for (float v : ch) total += v;
        if (total <= 0.0f) return {};
        return Analysis::estimateKey (ch);
    }

    juce::String toString (Flavour f)
    {
        switch (f)
        {
            case Flavour::followStyle: return "Follow the style";
            case Flavour::pop:         return "Pop";
            case Flavour::rock:        return "Rock";
            case Flavour::rnb:         return "R&B";
            case Flavour::hipHop:      return "Hip-Hop";
            case Flavour::trap:        return "Trap";
            case Flavour::indie:       return "Indie";
        }
        return {};
    }

    juce::String toString (Contour c)
    {
        switch (c)
        {
            case Contour::automatic: return "Automatic";
            case Contour::arch:      return "Arch (up, then home)";
            case Contour::rise:      return "Rise (build up)";
            case Contour::fall:      return "Fall (come down)";
            case Contour::wave:      return "Wave";
            case Contour::valley:    return "Valley (dip, then lift)";
            case Contour::hook:      return "Chant hook (stays close)";
        }
        return {};
    }

    juce::String toString (Variation v)
    {
        switch (v)
        {
            case Variation::subtle: return "Subtle (touch up what's there)";
            case Variation::fresh:  return "Fresh (new melody)";
            case Variation::wild:   return "Wild (surprise me)";
        }
        return {};
    }

    juce::String toString (Repetition r)
    {
        switch (r)
        {
            case Repetition::hooky:    return "Hooky (repeat the motif)";
            case Repetition::balanced: return "Balanced";
            case Repetition::free:     return "Free (always new)";
        }
        return {};
    }

    juce::String toString (Range r)
    {
        switch (r)
        {
            case Range::tight:  return "Tight (about a fifth)";
            case Range::normal: return "Normal (about an octave)";
            case Range::wide:   return "Wide (big chorus)";
        }
        return {};
    }

    juce::String toString (Register r)
    {
        switch (r)
        {
            case Register::asSung: return "Where it was sung";
            case Register::higher: return "A little higher";
            case Register::lower:  return "A little lower";
        }
        return {};
    }
}
