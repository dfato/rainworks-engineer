#include "TimeWarp.h"
#include <algorithm>
#include <cmath>

namespace ve
{
    namespace
    {
        double interp (const std::vector<TimeWarp::Anchor>& a, double x, bool fromOut) noexcept
        {
            auto key = [fromOut] (const TimeWarp::Anchor& p) { return fromOut ? p.out : p.in; };
            auto val = [fromOut] (const TimeWarp::Anchor& p) { return fromOut ? p.in : p.out; };
            if (a.empty()) return x;
            if (x <= key (a.front())) return val (a.front()) + (x - key (a.front()));
            if (x >= key (a.back())) return val (a.back()) + (x - key (a.back()));
            size_t lo = 0, hi = a.size() - 1;
            while (hi - lo > 1)
            {
                const size_t mid = (lo + hi) / 2;
                (key (a[mid]) <= x ? lo : hi) = mid;
            }
            const double k0 = key (a[lo]), k1 = key (a[hi]);
            const double t = k1 > k0 ? (x - k0) / (k1 - k0) : 0.0;
            return val (a[lo]) + t * (val (a[hi]) - val (a[lo]));
        }
    }

    double TimeWarp::inputAt (double t) const noexcept  { return interp (anchors, t, true); }
    double TimeWarp::outputAt (double u) const noexcept { return interp (anchors, u, false); }

    double TimeWarp::nearestGridPpq (double ppq, int grid, float swing) noexcept
    {
        const double step = 4.0 / (double) juce::jmax (1, grid);
        const double pair = 2.0 * step;
        const double base = std::floor (ppq / pair) * pair;
        const double swungOdd = step * (1.0 + (double) juce::jlimit (0.0f, 1.0f, swing) / 3.0);
        // Candidates: this pair's even + odd step, the next pair's even step, the previous odd step.
        const double cands[] = { base, base + swungOdd, base + pair, base - pair + swungOdd };
        double best = cands[0];
        for (double c : cands)
            if (std::abs (c - ppq) < std::abs (best - ppq)) best = c;
        return best;
    }

    bool TimeWarp::movesTime (const NotePlan& plan, const StyleProfile& style) noexcept
    {
        for (auto& n : plan.notes)
            if (n.timeShift != 0 || (style.quantize > 0.0f && n.correction > 0.0f))
                return true;
        return false;
    }

    std::vector<double> TimeWarp::targetOnsets (const NotePlan& plan, const StyleProfile& style, const TempoMap& tempo)
    {
        std::vector<double> out;
        out.reserve (plan.notes.size());
        for (auto& n : plan.notes)
        {
            double onset = (double) (n.start + n.timeShift);
            const double amount = (double) style.quantize * (double) juce::jlimit (0.0f, 1.0f, n.correction);
            if (amount > 0.0 && ! tempo.isEmpty())
            {
                const double q = tempo.ppqToSample (nearestGridPpq (tempo.sampleToPpq (onset), style.grid, style.swing));
                onset += amount * (q - onset);
            }
            out.push_back (onset);
        }
        return out;
    }

    TimeWarp TimeWarp::build (const NotePlan& plan, const StyleProfile& style, const TempoMap& tempo, std::int64_t numSamples)
    {
        TimeWarp w;
        if (numSamples <= 0 || ! movesTime (plan, style))
            return w;

        const auto onsets = targetOnsets (plan, style, tempo);
        const double len = (double) numSamples;

        // Anchors: region start, each note's start and end (moved by the same delta), region end.
        struct Raw { double in, out; };
        std::vector<Raw> raw;
        raw.push_back ({ 0.0, 0.0 });
        std::vector<size_t> order (plan.notes.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::sort (order.begin(), order.end(), [&] (size_t a, size_t b) { return plan.notes[a].start < plan.notes[b].start; });
        for (auto i : order)
        {
            const auto& n = plan.notes[i];
            const double s = juce::jlimit (0.0, len, (double) n.start);
            const double e = juce::jlimit (s, len, (double) n.end);
            const double d = onsets[i] - (double) n.start;
            if (s <= raw.back().in) continue; // overlapping/degenerate notes: keep the first
            raw.push_back ({ s, s + d });
            if (e > s) raw.push_back ({ e, e + d });
        }
        raw.push_back ({ len, len });

        // Keep gaps within the stretch limits: clamp forwards, then backwards from the fixed end.
        // Note bodies (start->end pairs) are allowed to be squeezed too if there's no other way.
        for (size_t i = 1; i < raw.size(); ++i)
        {
            const double gin = raw[i].in - raw[i - 1].in;
            raw[i].out = juce::jlimit (raw[i - 1].out + minStretch * gin, raw[i - 1].out + maxStretch * gin, raw[i].out);
        }
        raw.back().out = len;
        for (size_t i = raw.size() - 1; i-- > 0;)
        {
            const double gin = raw[i + 1].in - raw[i].in;
            raw[i].out = juce::jlimit (raw[i + 1].out - maxStretch * gin, raw[i + 1].out - minStretch * gin, raw[i].out);
        }
        raw.front().out = juce::jmax (0.0, raw.front().out);

        bool moved = false;
        for (auto& r : raw)
            moved = moved || std::abs (r.out - r.in) > 0.5;
        if (! moved)
            return w;

        for (auto& r : raw)
            w.anchors.push_back ({ r.in, r.out });
        return w;
    }
}
