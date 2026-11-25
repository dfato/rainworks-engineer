#include "TempoMap.h"
#include <algorithm>
#include <cmath>

namespace ve
{
    namespace
    {
        const TempoPoint fallbackPoint {};
        double samplesPerPpq (double sr, double bpm) { return sr * 60.0 / std::max (1.0, bpm); }
    }

    void TempoMap::addPoint (const TempoPoint& p)
    {
        if (! points.empty() && p.sample <= points.back().sample)
        {
            points.back() = p; // same position: last write wins
            return;
        }
        if (! points.empty())
        {
            const auto& b = points.back();
            if (std::abs (b.bpm - p.bpm) < 1.0e-9 && b.numerator == p.numerator && b.denominator == p.denominator)
                return; // no change worth recording
        }
        points.push_back (p);
    }

    const TempoPoint& TempoMap::pointAtSample (double sample) const noexcept
    {
        if (points.empty())
            return fallbackPoint;
        auto it = std::upper_bound (points.begin(), points.end(), sample,
                                    [] (double s, const TempoPoint& tp) { return s < (double) tp.sample; });
        return it == points.begin() ? points.front() : *(it - 1);
    }

    double TempoMap::sampleToPpq (double sample) const noexcept
    {
        const auto& p = pointAtSample (sample);
        return p.ppq + (sample - (double) p.sample) / samplesPerPpq (sr, p.bpm);
    }

    double TempoMap::ppqToSample (double ppq) const noexcept
    {
        if (points.empty())
            return ppq * samplesPerPpq (sr, 120.0);
        auto it = std::upper_bound (points.begin(), points.end(), ppq,
                                    [] (double q, const TempoPoint& tp) { return q < tp.ppq; });
        const auto& p = it == points.begin() ? points.front() : *(it - 1);
        return (double) p.sample + (ppq - p.ppq) * samplesPerPpq (sr, p.bpm);
    }
}
