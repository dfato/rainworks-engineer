#include "NotePlan.h"
#include <algorithm>
#include <cstring>

namespace ve
{
    bool NotePlan::isIdentity() const noexcept
    {
        for (auto& n : notes)
            if (n.correction > 0.0f && std::abs (n.targetMidi - n.detectedMidi) > 1.0e-4f)
                return false;
        return true;
    }

    std::uint64_t NotePlan::hash() const noexcept
    {
        std::uint64_t h = 1469598103934665603ull;
        auto mix = [&h] (const void* data, size_t n)
        {
            auto* p = static_cast<const unsigned char*> (data);
            for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
        };
        for (auto& n : notes)
        {
            if (n.correction <= 0.0f) continue; // uncorrected notes don't change the audio
            mix (&n.start, sizeof (n.start));
            mix (&n.end, sizeof (n.end));
            mix (&n.detectedMidi, sizeof (float));
            mix (&n.targetMidi, sizeof (float));
            mix (&n.correction, sizeof (float));
        }
        return h;
    }

    namespace NoteSegmenter
    {
        float snapToKey (float midi, const KeyEstimate& key) noexcept
        {
            const float nearest = std::round (midi);
            if (! key.isValid())
                return nearest;

            static constexpr int major[] = { 0, 2, 4, 5, 7, 9, 11 };
            static constexpr int minor[] = { 0, 2, 3, 5, 7, 8, 10 };
            const int* scale = key.minor ? minor : major;

            float best = nearest;
            float bestDist = 1.0e9f;
            for (int cand = (int) nearest - 2; cand <= (int) nearest + 2; ++cand)
            {
                const int pc = ((cand - key.tonic) % 12 + 12) % 12;
                if (std::find (scale, scale + 7, pc) == scale + 7) continue;
                const float dist = std::abs ((float) cand - midi);
                if (dist < bestDist) { bestDist = dist; best = (float) cand; }
            }
            return best;
        }

        void resnap (std::vector<Note>& notes, const KeyEstimate& key)
        {
            for (auto& n : notes)
                if (! n.userEdited)
                    n.targetMidi = snapToKey (n.detectedMidi, key);
        }

        namespace
        {
            float median (std::vector<float> v)
            {
                if (v.empty()) return 0.0f;
                auto mid = v.begin() + (std::ptrdiff_t) (v.size() / 2);
                std::nth_element (v.begin(), mid, v.end());
                return *mid;
            }
        }

        std::vector<Note> segment (const PitchTrack& track, const KeyEstimate& key, const Settings& s)
        {
            std::vector<Note> notes;
            const int n = (int) track.frames.size();
            if (n == 0 || track.sampleRate <= 0.0) return notes;

            const double frameMs = 1000.0 * track.hopSize / track.sampleRate;
            const int holdFrames = juce::jmax (1, (int) std::ceil (s.splitHoldMs / frameMs));
            const int minFrames = juce::jmax (2, (int) std::ceil (s.minNoteMs / frameMs));

            // Smoothed MIDI pitch (median over voiced neighbours), 0 = unvoiced.
            std::vector<float> midi ((size_t) n, 0.0f);
            const int half = s.medianFrames / 2;
            for (int i = 0; i < n; ++i)
            {
                if (! track.isVoiced (i)) continue;
                std::vector<float> w;
                for (int j = juce::jmax (0, i - half); j <= juce::jmin (n - 1, i + half); ++j)
                    if (track.isVoiced (j)) w.push_back (PitchTrack::hzToMidi (track.frames[(size_t) j].f0));
                midi[(size_t) i] = median (w);
            }

            auto emit = [&] (int a, int b) // frames [a, b)
            {
                if (b - a < minFrames) return;
                std::vector<float> vals;
                for (int i = a; i < b; ++i) if (midi[(size_t) i] > 0.0f) vals.push_back (midi[(size_t) i]);
                if ((int) vals.size() < minFrames / 2) return;
                Note note;
                note.start = (std::int64_t) std::llround (track.frameToSample (a) - track.hopSize * 0.5);
                note.end = (std::int64_t) std::llround (track.frameToSample (b) - track.hopSize * 0.5);
                note.start = std::max<std::int64_t> (0, note.start);
                note.detectedMidi = median (vals);
                note.targetMidi = snapToKey (note.detectedMidi, key);
                notes.push_back (note);
            };

            int noteStart = -1, gap = 0, devStart = -1;
            double sum = 0.0; int count = 0;
            for (int i = 0; i <= n; ++i)
            {
                const float m = i < n ? midi[(size_t) i] : 0.0f;

                if (m <= 0.0f)
                {
                    if (noteStart >= 0 && ++gap > s.maxGapFrames)
                    {
                        emit (noteStart, i - gap + 1);
                        noteStart = -1; devStart = -1;
                    }
                    continue;
                }

                gap = 0;
                if (noteStart < 0)
                {
                    noteStart = i; sum = m; count = 1; devStart = -1;
                    continue;
                }

                const float centre = (float) (sum / count);
                if (std::abs (m - centre) > s.splitSemitones)
                {
                    if (devStart < 0) devStart = i;
                    if (i - devStart + 1 >= holdFrames)
                    {
                        emit (noteStart, devStart);
                        noteStart = devStart;
                        sum = 0.0; count = 0;
                        for (int j = devStart; j <= i; ++j) if (midi[(size_t) j] > 0.0f) { sum += midi[(size_t) j]; ++count; }
                        devStart = -1;
                    }
                }
                else
                {
                    devStart = -1;
                    sum += m; ++count;
                }
            }
            return notes;
        }
    }
}
