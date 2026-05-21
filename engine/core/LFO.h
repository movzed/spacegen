#pragma once
// Lightweight LFO for animating light parameters (pan / tilt / intensity /
// hue / etc.). Per-instance; no global registry yet — that lands when the
// parameter-graph-spacegen skill ships.

#include <cmath>

namespace spacegen {

struct LFO {
    enum class Wave : int { Sine = 0, Triangle = 1, Square = 2, Saw = 3 };

    bool   enabled  = false;
    float  amplitude = 10.0f;   // peak deviation, in the target unit (deg / intensity / hue)
    float  freqHz   = 0.5f;     // cycles per second
    float  phase    = 0.0f;     // 0..1
    Wave   wave     = Wave::Sine;

    // Returns the modulation offset at time `t` (seconds).
    // Caller adds this to the base value.
    float eval(double t) const {
        if (!enabled) return 0.0f;
        // u is the fractional phase in [0, 1).
        double cycle = t * static_cast<double>(freqHz) + static_cast<double>(phase);
        double u = cycle - std::floor(cycle);
        float v = 0.0f;
        switch (wave) {
            case Wave::Sine:
                v = static_cast<float>(std::sin(u * 6.283185307179586));
                break;
            case Wave::Triangle:
                v = 4.0f * std::fabs(static_cast<float>(u) - 0.5f) - 1.0f;
                break;
            case Wave::Square:
                v = (u < 0.5) ? 1.0f : -1.0f;
                break;
            case Wave::Saw:
                v = 2.0f * static_cast<float>(u) - 1.0f;
                break;
        }
        return amplitude * v;
    }
};

} // namespace spacegen
