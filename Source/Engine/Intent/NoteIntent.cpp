#include "NoteIntent.h"
#include <algorithm>
#include <cmath>

namespace ve::NoteIntent
{
    namespace
    {
        float median (std::vector<float> v)
        {
            if (v.empty()) return 0.0f;
            auto mid = v.begin() + (std::ptrdiff_t) (v.size() / 2);
            std::nth_element (v.begin(), mid, v.end());
            return *mid;
        }

        bool inKey (int midi, const KeyEstimate& key)
        {
            static constexpr int major[] = { 0, 2, 4, 5, 7, 9, 11 };
            static constexpr int minor[] = { 0, 2, 3, 5, 7, 8, 10 };
            const int* scale = key.minor ? minor : major;
            const int pc = ((midi - key.tonic) % 12 + 12) % 12;
            return std::find (scale, scale + 7, pc) != scale + 7;
        }

        float takeMedianRms (const PitchTrack& t)
        {
            std::vector<float> v;
            for (auto& f : t.frames) if (f.f0 > 0.0f) v.push_back (f.rms);
            return median (v);
        }
    }

    Analysis analyse (const PitchTrack& t, const std::vector<Note>& notes, int index, const KeyEstimate& key,
                      float takeRms, const Settings& s)
    {
        Analysis out;
        const auto& n = notes[(size_t) index];
        out.target = NoteSegmenter::snapToKey (n.detectedMidi, key);
        if (t.hopSize <= 0 || t.sampleRate <= 0.0 || t.frames.empty()) return out;

        const double frameMs = 1000.0 * t.hopSize / t.sampleRate;
        const int a = juce::jlimit (0, (int) t.frames.size(), (int) (n.start / t.hopSize));
        const int b = juce::jlimit (a, (int) t.frames.size(), (int) ((n.end + t.hopSize - 1) / t.hopSize));
        std::vector<float> m, conf, rms;
        for (int i = a; i < b; ++i)
            if (t.frames[(size_t) i].f0 > 0.0f)
            {
                m.push_back (PitchTrack::hzToMidi (t.frames[(size_t) i].f0));
                conf.push_back (t.frames[(size_t) i].confidence);
                rms.push_back (t.frames[(size_t) i].rms);
            }
        if (m.size() < 3) return out;

        const float durMs = (float) ((double) (n.end - n.start) * 1000.0 / t.sampleRate);
        const int N = (int) m.size();

        // Core = middle 60%, where the note is "held".
        const int c0 = N / 5, c1 = juce::jmax (c0 + 1, N - N / 5);
        std::vector<float> core (m.begin() + c0, m.begin() + c1);
        const float centre = median (core);

        // Detrend the core (moving average ~250 ms, longer than a vibrato cycle) to find vibrato.
        const int win = juce::jmax (3, (int) std::lround (250.0 / frameMs));
        std::vector<float> resid (core.size());
        for (int i = 0; i < (int) core.size(); ++i)
        {
            const int lo = juce::jmax (0, i - win / 2), hi = juce::jmin ((int) core.size(), i + win / 2 + 1);
            double avg = 0.0;
            for (int j = lo; j < hi; ++j) avg += core[(size_t) j];
            resid[(size_t) i] = core[(size_t) i] - (float) (avg / (hi - lo));
        }
        double rss = 0.0;
        int crossings = 0;
        for (int i = 0; i < (int) resid.size(); ++i)
        {
            rss += (double) resid[(size_t) i] * resid[(size_t) i];
            if (i > 0 && (resid[(size_t) i] > 0.0f) != (resid[(size_t) i - 1] > 0.0f)) ++crossings;
        }
        const double coreSec = (double) core.size() * frameMs / 1000.0;
        const float residCents = 100.0f * (float) std::sqrt (rss / juce::jmax<size_t> (1, resid.size()));
        out.vibratoHz = coreSec > 0.0 ? (float) (crossings / 2.0 / coreSec) : 0.0f;
        out.vibratoCents = residCents * 1.414f;

        // Spread of the core around its centre (vibrato included).
        double spread = 0.0;
        for (float x : core) spread += (double) (x - centre) * (x - centre);
        const float spreadCents = 100.0f * (float) std::sqrt (spread / (double) core.size());

        const bool isVibrato = durMs >= s.vibratoMinMs && out.vibratoCents >= s.vibratoMinCents
                               && out.vibratoHz >= s.vibratoMinHz && out.vibratoHz <= s.vibratoMaxHz;
        if (isVibrato) out.flags |= vibrato;

        // Slide: start and end sit far apart and the pitch moves steadily between them.
        const int edge = juce::jmax (1, N / 5);
        std::vector<float> first (m.begin(), m.begin() + edge), last (m.end() - edge, m.end());
        const float travel = median (last) - median (first);
        if (std::abs (travel) >= s.slideSemitones)
        {
            // Correlation of pitch with time.
            double mt = 0.0, mm = 0.0, tt = 0.0, mean = 0.0;
            for (float x : m) mean += x;
            mean /= N;
            const double tmean = (N - 1) / 2.0;
            for (int i = 0; i < N; ++i) { mt += (i - tmean) * (m[(size_t) i] - mean); mm += (m[(size_t) i] - mean) * (m[(size_t) i] - mean); tt += (i - tmean) * (i - tmean); }
            const double r = (mm > 0.0 && tt > 0.0) ? mt / std::sqrt (mm * tt) : 0.0;
            if (std::abs (r) >= 0.8) out.flags |= slide;
        }

        // Scoop: begins clearly below, then holds.
        const int scoopFrames = juce::jmax (1, (int) std::lround (s.scoopMs / frameMs));
        if (! (out.flags & slide) && N > 2 * scoopFrames)
        {
            std::vector<float> start (m.begin(), m.begin() + scoopFrames);
            if (centre - median (start) >= s.scoopSemitones) out.flags |= scoop;
        }

        // Passing note: short and tucked between neighbours.
        if (durMs < s.passingMaxMs && index > 0 && index + 1 < (int) notes.size())
        {
            const double gapPrev = (double) (n.start - notes[(size_t) index - 1].end) * 1000.0 / t.sampleRate;
            const double gapNext = (double) (notes[(size_t) index + 1].start - n.end) * 1000.0 / t.sampleRate;
            if (gapPrev <= s.passingMaxGapMs && gapNext <= s.passingMaxGapMs) out.flags |= passing;
        }

        // Spoken / rapped: the tracker isn't sure, or the pitch wanders without being vibrato.
        // Only the first is left alone: a clearly pitched note that wanders is most often a
        // singer not holding the note, which is exactly what correction is for, so it is still
        // pulled in (most of the way; the drift itself is smoothed by the expression layer).
        const bool unpitched = median (conf) < s.spokenMinConfidence;
        if (unpitched || (! isVibrato && ! (out.flags & slide) && spreadCents > s.spokenMaxSpreadCents))
            out.flags |= spoken;

        // Quiet: ad-lib / ghost note.
        if (takeRms > 0.0f && median (rms) < takeRms * std::pow (10.0f, s.quietBelowDb / 20.0f))
            out.flags |= quiet;

        // Accidental: held, steady on a semitone the key doesn't have. Keep that semitone.
        const int nearest = (int) std::lround (centre);
        if (key.isValid() && durMs >= s.accidentalMinMs && ! (out.flags & (slide | spoken))
            && std::abs (centre - (float) nearest) * 100.0f <= s.accidentalMaxCents && ! inKey (nearest, key))
        {
            out.flags |= accidental;
            out.target = (float) nearest;
        }

        float scale = 1.0f;
        if (out.flags & spoken)  scale = juce::jmin (scale, unpitched ? 0.0f : 0.8f);
        if (out.flags & slide)   scale = juce::jmin (scale, 0.4f);
        if (out.flags & passing) scale = juce::jmin (scale, 0.5f);
        if (out.flags & quiet)   scale = juce::jmin (scale, 0.6f);
        if (out.flags & scoop)   scale = juce::jmin (scale, 0.8f);
        out.scale = scale;
        return out;
    }

    void apply (const PitchTrack& t, std::vector<Note>& notes, const KeyEstimate& key, const Settings& s)
    {
        const float takeRms = takeMedianRms (t);
        for (int i = 0; i < (int) notes.size(); ++i)
        {
            const auto a = analyse (t, notes, i, key, takeRms, s);
            auto& n = notes[(size_t) i];
            n.intent = a.flags;
            n.intentScale = a.scale;
            if (! n.userEdited)
                n.targetMidi = a.target;
        }
    }

    void clear (std::vector<Note>& notes, const KeyEstimate& key)
    {
        for (auto& n : notes)
        {
            n.intent = 0;
            n.intentScale = 1.0f;
            if (! n.userEdited)
                n.targetMidi = NoteSegmenter::snapToKey (n.detectedMidi, key);
        }
    }

    juce::String describe (std::uint8_t f)
    {
        juce::StringArray parts;
        if (f & slide)      parts.add ("slide");
        if (f & vibrato)    parts.add ("vibrato");
        if (f & scoop)      parts.add ("scoop");
        if (f & passing)    parts.add ("passing note");
        if (f & accidental) parts.add ("chosen accidental");
        if (f & spoken)     parts.add ("spoken");
        if (f & quiet)      parts.add ("ad-lib");
        auto s = parts.joinIntoString (", ");
        return s.isEmpty() ? s : s.substring (0, 1).toUpperCase() + s.substring (1);
    }
}
