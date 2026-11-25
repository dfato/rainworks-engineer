#pragma once

#include <cstdint>
#include <vector>

namespace ve
{
    // Tempo map local to one captured region. Sample 0 is the region start.
    // Each point holds the tempo in force from that sample onward.
    struct TempoPoint
    {
        std::int64_t sample = 0;   // offset from region start
        double ppq = 0.0;          // host PPQ at that sample
        double bpm = 120.0;
        int numerator = 4;
        int denominator = 4;
    };

    class TempoMap
    {
    public:
        TempoMap() = default;
        explicit TempoMap (double sampleRate) : sr (sampleRate) {}

        void setSampleRate (double s) noexcept { sr = s; }
        double getSampleRate() const noexcept { return sr; }

        // Points must be added in increasing sample order.
        void addPoint (const TempoPoint& p);
        const std::vector<TempoPoint>& getPoints() const noexcept { return points; }
        bool isEmpty() const noexcept { return points.empty(); }

        double sampleToPpq (double sample) const noexcept;
        double ppqToSample (double ppq) const noexcept;
        const TempoPoint& pointAtSample (double sample) const noexcept;

    private:
        double sr = 44100.0;
        std::vector<TempoPoint> points;
    };
}
