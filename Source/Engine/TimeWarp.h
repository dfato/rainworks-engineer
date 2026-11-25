#pragma once

#include "NotePlan.h"
#include "TempoMap.h"
#include "Style/StyleProfile.h"
#include <vector>

namespace ve
{
    // Monotonic piecewise-linear map between output time and input (sung) time,
    // region-local samples. Built from note onsets: each note keeps its length
    // and moves as a block; the gaps between notes stretch or squeeze to make room.
    struct TimeWarp
    {
        struct Anchor { double in = 0.0, out = 0.0; };
        std::vector<Anchor> anchors;   // increasing in both in and out; empty = identity

        bool isIdentity() const noexcept { return anchors.empty(); }
        double inputAt (double outSample) const noexcept;   // where output sample t reads from
        double outputAt (double inSample) const noexcept;   // where input sample u ends up

        // Gap stretch limits (output length / input length of a gap).
        static constexpr double minStretch = 0.25, maxStretch = 4.0;

        // Onset each note should move to: start + user nudge, then pulled toward the
        // (swung) grid by style.quantize * note.correction. `tempo` may be empty (no grid).
        static std::vector<double> targetOnsets (const NotePlan&, const StyleProfile&, const TempoMap& tempo);

        // True when this plan + style would move anything in time.
        static bool movesTime (const NotePlan&, const StyleProfile&) noexcept;

        static TimeWarp build (const NotePlan&, const StyleProfile&, const TempoMap& tempo, std::int64_t numSamples);

        // Nearest grid point in PPQ (grid = divisions per whole note, swing delays odd steps).
        static double nearestGridPpq (double ppq, int grid, float swing) noexcept;
    };
}
