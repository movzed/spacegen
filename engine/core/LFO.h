#pragma once
// Lightweight LFOs for animating light parameters.
//
// `LFO`         — single-axis: drives one number with a periodic wave.
// `MotionLFO`   — multi-axis: drives pan + tilt + intensity together with
//                 a stage-lighting motion pattern (figure-8, ballyhoo, etc.).
//
// Per-instance; no global registry yet — that lands when the
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
    // `extraPhase` lets callers offset the cycle (for per-fixture chases:
    // pass i/N to spread N fixtures evenly across the LFO cycle).
    float eval(double t, float extraPhase = 0.0f) const {
        if (!enabled) return 0.0f;
        // u is the fractional phase in [0, 1).
        double cycle = t * static_cast<double>(freqHz)
                     + static_cast<double>(phase)
                     + static_cast<double>(extraPhase);
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

// MotionLFO — drives pan + tilt + intensity together with a canonical
// stage-lighting motion pattern. Pick a pattern, set master frequency +
// amplitudes for each axis, plus phase. Multiple fixtures using the same
// pattern with different phases produce wave/fan effects across the rig.
struct MotionLFO {
    enum class Pattern : int {
        Off       = 0,
        Circle    = 1,  // pan = sin, tilt = cos
        Figure8   = 2,  // pan = sin, tilt = sin(2t)
        Ballyhoo  = 3,  // pseudo-random pan + tilt
        Wave      = 4,  // mostly horizontal pan with subtle tilt wobble
        Sweep     = 5,  // pan-only oscillation
        Can_Can   = 6,  // tilt rises sharply, falls slowly
        StrobePan = 7,  // pan + intensity strobe at higher freq
    };

    Pattern pattern   = Pattern::Off;
    float   freqHz    = 0.3f;
    float   panAmp    = 30.0f;   // degrees
    float   tiltAmp   = 15.0f;   // degrees
    float   intAmp    = 0.0f;    // intensity additive amplitude (units)
    float   phase     = 0.0f;    // 0..1, lets multiple fixtures sync with offset

    struct Output { float panDeg, tiltDeg, intensity; };

    Output eval(double t, float extraPhase = 0.0f) const {
        Output out{0.0f, 0.0f, 0.0f};
        if (pattern == Pattern::Off) return out;

        double cycle = t * static_cast<double>(freqHz)
                     + static_cast<double>(phase)
                     + static_cast<double>(extraPhase);
        double a     = cycle * 6.283185307179586;  // radians
        double s     = std::sin(a);
        double c     = std::cos(a);
        double s2    = std::sin(a * 2.0);

        switch (pattern) {
            case Pattern::Off:
                break;
            case Pattern::Circle:
                out.panDeg  = panAmp  * static_cast<float>(s);
                out.tiltDeg = tiltAmp * static_cast<float>(c);
                break;
            case Pattern::Figure8:
                out.panDeg  = panAmp  * static_cast<float>(s);
                out.tiltDeg = tiltAmp * static_cast<float>(s2);
                break;
            case Pattern::Ballyhoo: {
                double s1 = std::sin(a * 1.73);
                double s3 = std::sin(a * 2.41 + 1.7);
                out.panDeg  = panAmp  * static_cast<float>(0.7 * s + 0.3 * s1);
                out.tiltDeg = tiltAmp * static_cast<float>(0.7 * c + 0.3 * s3);
                break;
            }
            case Pattern::Wave:
                out.panDeg  = panAmp  * static_cast<float>(s);
                out.tiltDeg = tiltAmp * 0.3f * static_cast<float>(s2);
                break;
            case Pattern::Sweep:
                out.panDeg  = panAmp  * static_cast<float>(s);
                out.tiltDeg = 0.0f;
                break;
            case Pattern::Can_Can: {
                // Sharp upward kick + soft fall: |sin|^0.5 shaped
                double k = std::pow(std::fabs(s), 0.4) * ((s >= 0.0) ? 1.0 : -1.0);
                out.panDeg  = panAmp * 0.3f * static_cast<float>(std::sin(a * 0.5));
                out.tiltDeg = tiltAmp * static_cast<float>(k);
                break;
            }
            case Pattern::StrobePan:
                out.panDeg  = panAmp * static_cast<float>(s);
                out.tiltDeg = 0.0f;
                // Strobe intensity at 4× the pan freq
                out.intensity = intAmp * ((std::sin(a * 4.0) > 0.0) ? 1.0f : -1.0f);
                break;
        }
        return out;
    }
};

} // namespace spacegen
