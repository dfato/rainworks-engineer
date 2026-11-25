#include "ExpressionLayer.h"
#include "../Intent/NoteIntent.h"
#include "../PitchTracker.h"

namespace ve::ExpressionLayer
{
    namespace
    {
        // Stable 0..1 value per note, so a re-render puts runs on the same notes.
        double noteRandom (std::int64_t start)
        {
            auto z = (std::uint64_t) start + 0x9E3779B97F4A7C15ull;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            z ^= z >> 31;
            return (double) (z >> 11) / 9007199254740992.0;
        }

        // The notes the plan actually lands on (its scale), so runs move through the song's
        // own scale degrees rather than through chromatic steps.
        struct PlanScale
        {
            bool member[12] {};
            bool any = false;

            explicit PlanScale (const std::vector<Note>& notes)
            {
                double w[12] {}, total = 0.0;
                for (auto& n : notes)
                {
                    const int pc = ((int) std::lround (n.targetMidi) % 12 + 12) % 12;
                    const double len = (double) juce::jmax<std::int64_t> (1, n.end - n.start);
                    w[pc] += len; total += len;
                }
                int count = 0;
                for (int i = 0; i < 12; ++i)
                    if (total > 0.0 && w[i] >= 0.03 * total) { member[i] = true; ++count; }
                any = count >= 4;
            }

            // MIDI note `steps` scale degrees away from `midi` (whole tones without a scale).
            int step (int midi, int steps) const
            {
                if (steps == 0) return midi;
                if (! any) return midi + 2 * steps;
                const int dir = steps > 0 ? 1 : -1;
                int m = midi;
                for (int k = 0; k < std::abs (steps); ++k)
                {
                    int tries = 0;
                    do { m += dir; } while (! member[(m % 12 + 12) % 12] && ++tries < 4);
                }
                return m;
            }
        };
    }

    std::vector<float> buildShiftCurve (const NotePlan& plan, const StyleProfile& style,
                                        const std::vector<float>& f0, double sr)
    {
        const int n = (int) f0.size();
        std::vector<float> shift ((size_t) n, 0.0f);
        const auto& notes = plan.notes;
        const double msToSamples = sr / 1000.0;
        const int release = juce::jmax (1, (int) (10.0 * msToSamples));
        const float twoPi = juce::MathConstants<float>::twoPi;

        auto sungAt = [&] (int i, float fallback)
        {
            return f0[(size_t) i] > 0.0f ? PitchTrack::hzToMidi (f0[(size_t) i]) : fallback;
        };

        const PlanScale planScale (notes);
        // Phrase ends: the last corrected note before a pause (or the end).
        std::vector<char> phraseEnd (notes.size(), 0);
        for (size_t k = 0; k < notes.size(); ++k)
            phraseEnd[k] = k + 1 == notes.size() || (double) (notes[k + 1].start - notes[k].end) > 0.25 * sr;

        // Which notes slide into the next one.
        std::vector<char> slidesToNext (notes.size(), 0);
        if (style.slideMs > 0.0f)
            for (size_t k = 0; k + 1 < notes.size(); ++k)
                slidesToNext[k] = notes[k].correction > 0.0f && notes[k + 1].correction > 0.0f
                                  && (double) (notes[k + 1].start - notes[k].end) < style.slideMaxGapMs * msToSamples;

        for (size_t k = 0; k < notes.size(); ++k)
        {
            const auto& note = notes[k];
            const float c = juce::jlimit (0.0f, 1.0f, note.correction);
            if (c <= 0.0f) continue;
            const float expressionAmount = juce::jmin (1.0f, 2.0f * c);

            const int a = (int) juce::jlimit<std::int64_t> (0, n, note.start);
            const int b = (int) juce::jlimit<std::int64_t> (0, n, note.end);
            if (b <= a) continue;

            // A slow retune is a style's lazy glide onto the note, but every note still has to
            // arrive: the glide takes at most 15% of the note, so short notes land in tune too.
            const double retuneSamples = juce::jmin ((double) style.retuneMs * msToSamples, 0.15 * (b - a));
            const double scoopSamples = juce::jmin ((double) style.scoopMs * msToSamples, (b - a) * 0.4);
            const double vibOnset = style.vibratoOnsetMs * msToSamples;
            const bool fadeOut = ! slidesToNext[k];
            // Whole semitones between the nearest semitone and the target (a style's scale, or
            // an out-of-key note, moving it to another degree) are applied in full: half a
            // semitone of a move sounds out of tune. Correct scales only the fine tuning.
            const float jump = note.targetMidi - std::round (note.detectedMidi);
            // Stage 4: the singer's own vibrato is kept on notes where it was intended,
            // unless the style deliberately flattens everything (hard tune, retune < 10 ms).
            // On notes without intended vibrato, the wobble that is left is a singer not holding
            // the note steady: correction smooths it out too (to a quarter of the style's amount at
            // 100%), so a shaky take comes out polished while each style keeps its own character.
            const bool intendedVibrato = (note.intent & NoteIntent::vibrato) != 0;
            const float keep = intendedVibrato && style.retuneMs >= 10.0f
                                   ? juce::jmax (style.keepVariation, 0.8f)
                                   : style.keepVariation * (1.0f - 0.75f * c);

            // Ornaments. A run is a quick line of scale notes sung on one syllable: in the middle
            // of a phrase it turns around the note and comes back, at a phrase end it walks down
            // and lands on a lower scale note. A fall drops off the end of a phrase.
            std::vector<float> run;
            int runFrom = b;
            {
                // Shorter notes get a shorter, quicker run rather than none.
                double runNote = style.runNoteMs * msToSamples;
                const double tail = (phraseEnd[k] ? 0.14 : 0.07) * sr;
                const double room = (double) (b - a) - tail - 0.12 * sr;
                const int fewest = juce::jmin (3, style.runNotes);
                int count = juce::jmin (style.runNotes, (int) (room / runNote));
                if (count < fewest)
                {
                    runNote *= 0.75;
                    count = juce::jmin (style.runNotes, (int) (room / runNote));
                }
                const double chance = style.runs * (phraseEnd[k] ? 1.4 : 0.6);
                if (style.runs > 0.0f && count >= fewest && noteRandom (note.start) < chance)
                {
                    const int root = (int) std::lround (note.targetMidi);
                    std::vector<float> offs;
                    for (int j = 0; j < count; ++j)
                    {
                        const int deg = phraseEnd[k] ? 1 - j                       // +1, 0, -1, -2 ...
                                                     : (j % 4 == 0 ? 1 : j % 4 == 2 ? -1 : 0); // +1, 0, -1, 0 ...
                        offs.push_back ((float) (planScale.step (root, deg) - root));
                    }
                    const float landing = phraseEnd[k] ? offs.back() : 0.0f;
                    runFrom = (int) (b - runNote * count - tail);
                    run.assign ((size_t) (b - runFrom), landing);
                    for (int i = 0; i < b - runFrom; ++i)
                    {
                        const double pos = i / runNote;
                        const int j = (int) pos;
                        const float to = j < count ? offs[(size_t) j] : landing;
                        const float from = j == 0 ? 0.0f : j - 1 < count ? offs[(size_t) j - 1] : landing;
                        // Move to each note in the first 35% of its slot, then hold it.
                        const float w = (float) juce::jlimit (0.0, 1.0, (pos - j) / 0.35);
                        run[(size_t) i] = from + (to - from) * w * w * (3.0f - 2.0f * w);
                    }
                }
            }
            const bool falls = run.empty() && phraseEnd[k] && style.fallCents > 0.0f && style.fallMs > 0.0f
                               && (double) (b - a) >= (style.fallMs + 150.0) * msToSamples;
            const double fallSamples = style.fallMs * msToSamples;

            // The sung pitch, split in two: drift (the slow wander of a singer who is not quite
            // holding the note, under ~2 Hz) and modulation (vibrato and fast detail). Correction
            // removes the drift, so a shaky held note comes out steady and on pitch; the style's
            // keepVariation decides how much modulation survives. Pitch-tracker glitches
            // (octave jumps, a consonant's noise) further than 1.5 semitones from the note
            // centre are treated as the centre, so they never become jumps.
            const int len = b - a;
            std::vector<float> sung ((size_t) len), drift ((size_t) len);
            for (int i = 0; i < len; ++i)
            {
                float d = sungAt (a + i, note.detectedMidi);
                if (std::abs (d - note.detectedMidi) > 1.5f) d = note.detectedMidi;
                sung[(size_t) i] = d;
            }
            {
                // Two box averages, 200 and 250 ms: they null 4-8 Hz vibrato almost exactly.
                auto box = [len] (const std::vector<float>& in, std::vector<float>& out, int half)
                {
                    std::vector<double> sum ((size_t) len + 1, 0.0);
                    for (int i = 0; i < len; ++i) sum[(size_t) i + 1] = sum[(size_t) i] + in[(size_t) i];
                    for (int i = 0; i < len; ++i)
                    {
                        const int lo = juce::jmax (0, i - half), hi = juce::jmin (len, i + half + 1);
                        out[(size_t) i] = (float) ((sum[(size_t) hi] - sum[(size_t) lo]) / (double) (hi - lo));
                    }
                };
                std::vector<float> tmp ((size_t) len);
                box (sung, tmp, (int) (0.100 * sr));
                box (tmp, drift, (int) (0.125 * sr));
            }
            const float keepDrift = keep * (1.0f - c);

            for (int i = a; i < b; ++i)
            {
                const double t = i - a;
                const float d = sung[(size_t) (i - a)];
                const float dr = drift[(size_t) (i - a)];
                const float flat = note.targetMidi + keep * (d - dr) + keepDrift * (dr - note.detectedMidi);
                const float retune = retuneSamples <= 0.0 ? 1.0f : (float) (1.0 - std::exp (-t / retuneSamples));

                // The style's own expression (scoop, added vibrato) is not a correction, so it is
                // not scaled down with the correction amount: a loose style like Gospel or
                // Neo-Soul still gets its full vibrato and scoops. It fades in over the first
                // half of the Correct range so 0% stays exactly as sung.
                float expression = 0.0f;
                if (style.scoopCents > 0.0f && t < scoopSamples)
                    expression -= style.scoopCents / 100.0f * (float) (1.0 - t / scoopSamples);

                if (style.vibratoDepthCents > 0.0f && t > vibOnset)
                {
                    const float tv = (float) ((t - vibOnset) / sr);
                    const float ramp = juce::jmin (1.0f, tv / 0.2f);
                    // No vibrato while a run is moving.
                    const float quiet = i >= runFrom ? 0.0f : 1.0f;
                    expression += quiet * style.vibratoDepthCents / 100.0f * ramp * std::sin (twoPi * style.vibratoRateHz * tv);
                }
                if (i >= runFrom)
                    expression += run[(size_t) (i - runFrom)];
                if (falls && (double) (b - i) < fallSamples)
                {
                    const float x = (float) (1.0 - (b - i) / fallSamples);
                    expression -= style.fallCents / 100.0f * x * x;
                }

                float s = retune * (c * (flat - jump - d) + expressionAmount * jump) + expressionAmount * expression;
                if (fadeOut && b - 1 - i < release)
                    s *= (float) (b - 1 - i) / (float) release;
                shift[(size_t) i] = s;
            }
        }

        // Slides: across the join between two corrected notes, glide from one target to the other.
        const int halfSlide = (int) (style.slideMs * 0.5 * msToSamples);
        for (size_t k = 0; k + 1 < notes.size() && halfSlide > 0; ++k)
        {
            if (! slidesToNext[k]) continue;
            const auto& x = notes[k];
            const auto& y = notes[k + 1];
            const int from = (int) juce::jmax<std::int64_t> (x.start, x.end - halfSlide);
            const int to = (int) juce::jmin<std::int64_t> (y.end, y.start + halfSlide);
            if (to <= from) continue;

            for (int i = juce::jmax (0, from); i < juce::jmin (n, to); ++i)
            {
                const float w = (float) (i - from) / (float) (to - from);
                const float smooth = w * w * (3.0f - 2.0f * w);
                const float line = x.targetMidi + smooth * (y.targetMidi - x.targetMidi);
                if (i < x.end)      shift[(size_t) i] += x.correction * (line - x.targetMidi);
                else if (i >= y.start) shift[(size_t) i] += y.correction * (line - y.targetMidi);
            }
        }
        // Smooth the curve with a zero-phase low-pass (two one-pole passes, ~5 ms each way).
        // Vibrato (4-8 Hz) passes untouched; what is removed is frame-to-frame tracker
        // jitter and the hard edges at note starts, which PSOLA turns into clicks.
        if (n > 1)
        {
            const float a = std::exp (-1.0f / (0.005f * (float) sr));
            float y = shift[0];
            for (int i = 0; i < n; ++i) { y = shift[(size_t) i] + a * (y - shift[(size_t) i]); shift[(size_t) i] = y; }
            y = shift[(size_t) n - 1];
            for (int i = n - 1; i >= 0; --i) { y = shift[(size_t) i] + a * (y - shift[(size_t) i]); shift[(size_t) i] = y; }
        }
        return shift;
    }
}
