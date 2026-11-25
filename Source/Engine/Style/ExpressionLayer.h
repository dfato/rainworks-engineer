#pragma once

#include "StyleProfile.h"
#include "../NotePlan.h"

namespace ve::ExpressionLayer
{
    // Turns a note plan + style into a per-sample pitch shift (semitones) to
    // apply to the captured take. Only notes with correction > 0 are touched.
    //
    // For each corrected note, with d(t) the sung pitch and T the target:
    //   flat(t)   = T + keepVariation * (d(t) - noteCentre)        the "ideal" line
    //   styled(t) = d(t) + retune(t) * (flat(t) - d(t))           glide in at retuneMs
    //               + scoop at the start + added vibrato on long notes
    //   shift(t)  = correction * (styled(t) - d(t))
    // and short gaps between corrected notes become slides from one target to the next.
    std::vector<float> buildShiftCurve (const NotePlan&, const StyleProfile&,
                                        const std::vector<float>& f0PerSample, double sampleRate);
}
