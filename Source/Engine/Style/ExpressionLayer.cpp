#include "ExpressionLayer.h"
#include "../Intent/NoteIntent.h"
#include "../PitchTracker.h"

namespace ve::ExpressionLayer
{
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

            const double retuneSamples = style.retuneMs * msToSamples;
            const double scoopSamples = juce::jmin ((double) style.scoopMs * msToSamples, (b - a) * 0.4);
            const double vibOnset = style.vibratoOnsetMs * msToSamples;
            const bool fadeOut = ! slidesToNext[k];
            // Whole semitones between the nearest semitone and the target (a style's scale, or
            // an out-of-key note, moving it to another degree) are applied in full: half a
            // semitone of a move sounds out of tune. Correct scales only the fine tuning.
            const float jump = note.targetMidi - std::round (note.detectedMidi);
            // Stage 4: the singer's own vibrato is kept on notes where it was intended,
            // unless the style deliberately flattens everything (hard tune, retune < 10 ms).
            const float keep = (note.intent & NoteIntent::vibrato) != 0 && style.retuneMs >= 10.0f
                                   ? juce::jmax (style.keepVariation, 0.8f) : style.keepVariation;

            for (int i = a; i < b; ++i)
            {
                const double t = i - a;
                const float d = sungAt (i, note.detectedMidi);
                const float flat = note.targetMidi + keep * (d - note.detectedMidi);
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
                    expression += style.vibratoDepthCents / 100.0f * ramp * std::sin (twoPi * style.vibratoRateHz * tv);
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
        return shift;
    }
}
