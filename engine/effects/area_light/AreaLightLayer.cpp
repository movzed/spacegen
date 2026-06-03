#include "AreaLightLayer.h"

#include "Scene.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "imgui.h"

#include <algorithm>
#include <cmath>

namespace spacegen {

namespace {

constexpr float kPi  = 3.14159265358979323846f;
constexpr float kTau = 6.28318530717958647692f;

// Convert pan/tilt (deg) into a world-space direction.  Same convention as
// DirectionalLightLayer: pan = yaw around +Z (Z-up), 0 = +Y; tilt = pitch
// from the horizontal plane (positive = up).
inline glm::vec3 panTiltToDir(float panDeg, float tiltDeg) {
    float pr = panDeg  * kPi / 180.0f;
    float tr = tiltDeg * kPi / 180.0f;
    float cp = std::cos(pr), sp = std::sin(pr);
    float ct = std::cos(tr), st = std::sin(tr);
    return glm::vec3(-sp * ct, cp * ct, st);
}

// Cheap xorshift32 — deterministic, not for crypto. Stateful PRNG used for
// the random-orbit mode so the operator's path is reproducible across runs
// (modulo `dwellSeconds * elapsedSeconds` driving the resample clock).
inline uint32_t xorshift32(uint32_t& s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

inline float urand(uint32_t& s) {
    // [0, 1)
    return static_cast<float>(xorshift32(s) & 0x00FFFFFFu)
         / static_cast<float>(0x01000000u);
}

// Uniform direction on the unit sphere via Marsaglia's method (1972).
// Rejects samples outside the unit disc, then lifts to 3D — produces
// strictly uniform area distribution, no pole bias.
inline glm::vec3 randomOnUnitSphere(uint32_t& seed) {
    for (int tries = 0; tries < 32; ++tries) {
        float u = urand(seed) * 2.0f - 1.0f;   // [-1, 1)
        float v = urand(seed) * 2.0f - 1.0f;
        float s = u * u + v * v;
        if (s >= 1.0f || s < 1e-6f) continue;  // reject
        float f = 2.0f * std::sqrt(1.0f - s);
        return glm::vec3(u * f, v * f, 1.0f - 2.0f * s);
    }
    // Astronomically unlikely; trivial fallback.
    return glm::vec3(0.0f, 0.0f, 1.0f);
}

// Slerp between two unit-length vectors (a, b). When the two are nearly
// parallel we fall back to linear interpolation + renormalisation to avoid
// the 1/sin(θ) singularity.
inline glm::vec3 slerp(const glm::vec3& a, const glm::vec3& b, float t) {
    float d = glm::clamp(glm::dot(a, b), -1.0f, 1.0f);
    if (d > 0.9995f) {
        return glm::normalize(glm::mix(a, b, t));
    }
    float theta = std::acos(d);
    float sinT  = std::sin(theta);
    float wa    = std::sin((1.0f - t) * theta) / sinT;
    float wb    = std::sin(t        * theta) / sinT;
    return glm::normalize(a * wa + b * wb);
}

// Smoothstep-ish ease curve, 0..1 in, 0..1 out, parameterised by `mix`.
// mix == 0 → linear,  mix == 1 → 0.5 - 0.5·cos(π·u).
inline float easeMix(float u, float mix) {
    u = glm::clamp(u, 0.0f, 1.0f);
    float cosineEased = 0.5f - 0.5f * std::cos(u * kPi);
    return glm::mix(u, cosineEased, glm::clamp(mix, 0.0f, 1.0f));
}

// Discrete easing selector applied to a 0..1 phase. All curves map
// [0,1]→[0,1] with f(0)=0, f(1)=1 so the period seam stays continuous
// when the eased phase is fed back into a 2π angle.
inline float applyEasing(float u, AreaLightLayer::Easing e) {
    u = glm::clamp(u, 0.0f, 1.0f);
    switch (e) {
        case AreaLightLayer::Easing::Linear:
            return u;
        case AreaLightLayer::Easing::Smoothstep:
            // Hermite 3u²-2u³ — classic GLSL smoothstep, C¹ at the ends.
            return u * u * (3.0f - 2.0f * u);
        case AreaLightLayer::Easing::EaseInOut:
            // Sinusoidal ease, equivalent to easeMix(u, 1).
            return 0.5f - 0.5f * std::cos(u * kPi);
        case AreaLightLayer::Easing::Bounce: {
            // Penner-style bounce, but normalised so f(1)==1 exactly. The
            // light "lands" with a couple of decaying overshoots — reads as
            // an articulated tap at the end of each swing.
            const float n1 = 7.5625f, d1 = 2.75f;
            float x = u;
            if (x < 1.0f / d1)        x = n1 * x * x;
            else if (x < 2.0f / d1) { x -= 1.5f  / d1; x = n1 * x * x + 0.75f; }
            else if (x < 2.5f / d1) { x -= 2.25f / d1; x = n1 * x * x + 0.9375f; }
            else                    { x -= 2.625f/ d1; x = n1 * x * x + 0.984375f; }
            return x;
        }
    }
    return u;
}

// Compute the scene's *safe orbit radius*: half the diagonal of the
// world-space AABB (circumscribed-sphere radius), scaled by safetyFactor,
// floored at a sensible minimum so empty scenes don't collapse to a point.
inline float computeSafeRadius(const Scene* scene, float safetyFactor) {
    if (!scene) return 2.0f;
    glm::vec3 extent = scene->bboxMax - scene->bboxMin;
    float diag       = glm::length(extent);
    // Half-diagonal is the radius of the AABB's circumscribed sphere.
    float circumR    = 0.5f * diag;
    // We could use std::max({ext.x, ext.y, ext.z}) * 0.5f instead (the
    // inscribed sphere of the *box of half-extents*), but circumscribed is
    // strictly safer — for very anisotropic structures (a long catwalk) it
    // keeps the light away from the far ends, not just the middle.
    float baseRadius = std::max(circumR * safetyFactor, 0.25f);
    return baseRadius;
}

// Build an orthonormal frame (right, up) on the unit sphere tangent plane
// at the "north pole" of the chosen orbit axis. Used by Lissajous to lift
// a 2D parametric curve into 3D without singular behaviour.
inline void axisFrame(AreaLightLayer::Axis a,
                      glm::vec3& right, glm::vec3& up, glm::vec3& fwd) {
    switch (a) {
        case AreaLightLayer::Axis::X:
            fwd   = glm::vec3(1.0f, 0.0f, 0.0f);
            right = glm::vec3(0.0f, 1.0f, 0.0f);
            up    = glm::vec3(0.0f, 0.0f, 1.0f);
            break;
        case AreaLightLayer::Axis::Y:
            fwd   = glm::vec3(0.0f, 1.0f, 0.0f);
            right = glm::vec3(0.0f, 0.0f, 1.0f);
            up    = glm::vec3(1.0f, 0.0f, 0.0f);
            break;
        case AreaLightLayer::Axis::Z:
        default:
            fwd   = glm::vec3(0.0f, 0.0f, 1.0f);
            right = glm::vec3(1.0f, 0.0f, 0.0f);
            up    = glm::vec3(0.0f, 1.0f, 0.0f);
            break;
    }
}

} // anon namespace

// ---------------------------------------------------------------------------
// Construction / boilerplate
// ---------------------------------------------------------------------------

AreaLightLayer::AreaLightLayer() {
    name      = "Area Light";
    blendMode = BlendMode::Add;
    colorTag  = glm::vec3(0.70f, 0.95f, 1.00f);
    intensityLFO.amplitude = 1.5f;
    intensityLFO.freqHz    = 0.15f;
}

float AreaLightLayer::equivalentRadius() const {
    if (shape == Shape::Disc) return std::max(radius, 1e-4f);
    // Equivalent-area disc — Karis 2013 § 4.3 suggests this as a cheap
    // disc<->rectangle bridge that "just works" for moving stage lights.
    float area = std::max(width, 1e-4f) * std::max(height, 1e-4f);
    return std::sqrt(area / kPi);
}

// ---------------------------------------------------------------------------
// Orbit parametric paths — all return a *unit-length* direction on the
// sphere. World position is centroid + dir * effectiveRadius.
// ---------------------------------------------------------------------------

glm::vec3 AreaLightLayer::evalCircular_(double t) const {
    float phi = static_cast<float>(t) * speedDegPerSec * kPi / 180.0f;
    float c = std::cos(phi), s = std::sin(phi);
    // Each axis selection produces an orbit *around* that axis.
    switch (orbitAxis) {
        case Axis::Z: return glm::vec3(c, s, 0.0f);   // orbit in XY
        case Axis::Y: return glm::vec3(c, 0.0f, s);   // orbit in XZ
        case Axis::X: return glm::vec3(0.0f, c, s);   // orbit in YZ
    }
    return glm::vec3(0.0f, 0.0f, 1.0f);
}

glm::vec3 AreaLightLayer::evalLissajous_(double t) const {
    glm::vec3 right, up, fwd;
    axisFrame(orbitAxis, right, up, fwd);
    float ft = static_cast<float>(t);
    float xx = lissAx * std::sin(lissA * ft);
    float yy = lissAy * std::sin(lissB * ft + lissPhi);
    // Tangent-plane vector at the axis pole, then normalised — projects
    // onto the unit sphere. The trajectory traces the classic Lissajous
    // figure rolled onto the sphere; never crosses the centroid because
    // we always renormalise.
    glm::vec3 v = fwd + right * xx + up * yy;
    if (glm::length(v) < 1e-5f) v = fwd;
    return glm::normalize(v);
}

glm::vec3 AreaLightLayer::evalHelix_(double t) const {
    float phi  = static_cast<float>(t) * speedDegPerSec * kPi / 180.0f;
    float zRaw = bobAmp * std::sin(kTau * bobFreqHz * static_cast<float>(t));
    float zz   = glm::clamp(zRaw, -0.99f, 0.99f);
    float r    = std::sqrt(std::max(1.0f - zz * zz, 0.0f));
    float c    = r * std::cos(phi);
    float s    = r * std::sin(phi);
    // Apply orbit axis: zz goes along the chosen axis, (c, s) on the
    // tangent plane.
    switch (orbitAxis) {
        case Axis::Z: return glm::vec3(c,  s,  zz);
        case Axis::Y: return glm::vec3(c,  zz, s);
        case Axis::X: return glm::vec3(zz, c,  s);
    }
    return glm::vec3(c, s, zz);
}

glm::vec3 AreaLightLayer::evalRandom_(double t) {
    if (!randomInited_) {
        randomPrev_   = randomOnUnitSphere(randomSeed_);
        randomNext_   = randomOnUnitSphere(randomSeed_);
        randomT0_     = t;
        randomInited_ = true;
    }
    double dwell = std::max(0.05, static_cast<double>(dwellSeconds));
    double dt    = t - randomT0_;
    while (dt >= dwell) {
        randomPrev_ = randomNext_;
        randomNext_ = randomOnUnitSphere(randomSeed_);
        randomT0_  += dwell;
        dt         -= dwell;
    }
    float u = static_cast<float>(dt / dwell);
    u = easeMix(u, slerpEase);
    return slerp(randomPrev_, randomNext_, u);
}

// ---------------------------------------------------------------------------
// Articulated paths
//
// pathAngle_(): turns elapsed time into an eased, phase-offset angle. The
// raw phase advances at `speedDegPerSec` (one "cycle" = 360°). We add the
// per-light `phaseOffset` (in turns), fold the integer turns out, ease the
// fractional 0..1 phase through the selected curve, then re-expand to an
// absolute angle (whole turns + eased fraction)·2π. This keeps motion
// monotonic across periods while letting Smoothstep/EaseInOut/Bounce shape
// the *within-cycle* timing — so staggered-phase lights articulate as a
// choreographed group rather than sliding at constant rate.
// ---------------------------------------------------------------------------

float AreaLightLayer::pathAngle_(double t) const {
    double turns = t * (static_cast<double>(speedDegPerSec) / 360.0)
                 + static_cast<double>(phaseOffset);
    double whole = std::floor(turns);
    float  frac  = static_cast<float>(turns - whole);    // [0,1)
    float  eased = applyEasing(frac, easing);            // [0,1], f(0)=0,f(1)=1
    return static_cast<float>(whole + static_cast<double>(eased)) * kTau;
}

// Figure-eight (Gerono lemniscate): x = sin θ, y = sin θ·cos θ. Laid on the
// axis tangent plane and lifted to the sphere. Traces a crossing "∞" loop
// over the structure — a sweep that returns through its own centre line.
glm::vec3 AreaLightLayer::evalFigureEight_(double t) const {
    glm::vec3 right, up, fwd;
    axisFrame(orbitAxis, right, up, fwd);
    float th = pathAngle_(t);
    float xx = pathAmplitude * std::sin(th);
    float yy = pathAmplitude * std::sin(th) * std::cos(th);
    glm::vec3 v = fwd + right * xx + up * yy;
    if (glm::length(v) < 1e-5f) v = fwd;
    return glm::normalize(v);
}

// Rose / rhodonea: r(θ) = cos(k·θ) traces a k-petal (odd k) or 2k-petal
// (even k) star. The petals bloom outward and snap back through the pole,
// giving a punchy articulated "flower" sweep.
glm::vec3 AreaLightLayer::evalRose_(double t) const {
    glm::vec3 right, up, fwd;
    axisFrame(orbitAxis, right, up, fwd);
    float th = pathAngle_(t);
    int   k  = std::max(1, roseK);
    float r  = pathAmplitude * std::cos(static_cast<float>(k) * th);
    float xx = r * std::cos(th);
    float yy = r * std::sin(th);
    glm::vec3 v = fwd + right * xx + up * yy;
    if (glm::length(v) < 1e-5f) v = fwd;
    return glm::normalize(v);
}

// Spiral: the azimuth sweeps steadily while the *polar* opening breathes
// in and out (cos over `spiralTurns` excursions per cycle). The light winds
// from near the pole out toward the equator and back — a coiling orbit.
glm::vec3 AreaLightLayer::evalSpiral_(double t) const {
    float th    = pathAngle_(t);
    // Polar angle oscillates 0..(spiralPolar·π/2): 0 = at the axis pole,
    // larger = swung toward the equator. cos keeps it smooth & bounded.
    float breath = 0.5f - 0.5f * std::cos(spiralTurns * th);   // 0..1
    float polar  = glm::clamp(spiralPolar, 0.0f, 1.0f) * (kPi * 0.5f) * breath;
    float zz = std::cos(polar);                 // axis component (near 1 at pole)
    float rr = std::sin(polar);                 // tangent-plane radius
    float c  = rr * std::cos(th);
    float s  = rr * std::sin(th);
    switch (orbitAxis) {
        case Axis::Z: return glm::vec3(c,  s,  zz);
        case Axis::Y: return glm::vec3(c,  zz, s);
        case Axis::X: return glm::vec3(zz, c,  s);
    }
    return glm::vec3(c, s, zz);
}

// Pendulum: a damped harmonic swing about the axis pole. The swing angle is
// A·e^(-ζ·φ)·cos(φ) where φ is the (un-eased) free-running phase, so the arc
// decays toward the pole then the cycle reseeds — a articulated rocking
// motion. Swings within the tangent plane along `right`.
glm::vec3 AreaLightLayer::evalPendulum_(double t) const {
    glm::vec3 right, up, fwd;
    axisFrame(orbitAxis, right, up, fwd);
    // Free-running phase (no path-angle easing here; the damping shapes it).
    double turns = t * (static_cast<double>(speedDegPerSec) / 360.0)
                 + static_cast<double>(phaseOffset);
    float  frac  = static_cast<float>(turns - std::floor(turns));   // 0..1
    float  phi   = frac * kTau;
    float  decay = std::exp(-glm::clamp(pendDamping, 0.0f, 4.0f) * frac);
    float  swing = glm::radians(pendSwingDeg) * decay * std::cos(phi);
    // Rotate `fwd` toward `right` by the swing angle: stays on the sphere.
    glm::vec3 v = fwd * std::cos(swing) + right * std::sin(swing);
    return glm::normalize(v);
}

// Compound: layer two harmonics on the tangent plane — a slow primary
// circle plus a faster secondary loop (freq = compoundRatio), blended by
// compoundMix. Non-integer ratios give a quasi-periodic woven path that
// never quite repeats, reading as organic articulated drift.
glm::vec3 AreaLightLayer::evalCompound_(double t) const {
    glm::vec3 right, up, fwd;
    axisFrame(orbitAxis, right, up, fwd);
    float th  = pathAngle_(t);
    float th2 = th * compoundRatio;
    float mix = glm::clamp(compoundMix, 0.0f, 1.0f);
    float xx = pathAmplitude * ((1.0f - mix) * std::cos(th) + mix * std::cos(th2));
    float yy = pathAmplitude * ((1.0f - mix) * std::sin(th) + mix * std::sin(th2));
    glm::vec3 v = fwd + right * xx + up * yy;
    if (glm::length(v) < 1e-5f) v = fwd;
    return glm::normalize(v);
}

// ---------------------------------------------------------------------------
// Motion presets — one-click choreography bundles.
// ---------------------------------------------------------------------------

void AreaLightLayer::applyPreset(MotionPreset p) {
    preset = p;
    switch (p) {
        case MotionPreset::Custom:
            break;   // leave operator values as-is
        case MotionPreset::SlowOrbit:
            orbitMode      = OrbitMode::Circular;
            orbitAxis      = Axis::Z;
            speedDegPerSec = 9.0f;
            easing         = Easing::Linear;
            phaseOffset    = 0.0f;
            break;
        case MotionPreset::Figure8Sweep:
            orbitMode      = OrbitMode::FigureEight;
            orbitAxis      = Axis::Z;
            speedDegPerSec = 26.0f;
            pathAmplitude  = 1.0f;
            easing         = Easing::EaseInOut;
            phaseOffset    = 0.0f;
            break;
        case MotionPreset::RoseBloom:
            orbitMode      = OrbitMode::Rose;
            orbitAxis      = Axis::Z;
            speedDegPerSec = 20.0f;
            pathAmplitude  = 1.1f;
            roseK          = 5;
            easing         = Easing::Smoothstep;
            phaseOffset    = 0.0f;
            break;
        case MotionPreset::PendulumPair:
            orbitMode      = OrbitMode::Pendulum;
            orbitAxis      = Axis::Z;
            speedDegPerSec = 30.0f;
            pendSwingDeg   = 70.0f;
            pendDamping    = 0.5f;
            easing         = Easing::Linear;
            // Caller staggers phaseOffset (e.g. 0.0 and 0.5) across the pair.
            phaseOffset    = 0.0f;
            break;
        case MotionPreset::ChaosSwarm:
            orbitMode      = OrbitMode::Compound;
            orbitAxis      = Axis::Z;
            speedDegPerSec = 34.0f;
            pathAmplitude  = 1.2f;
            compoundRatio  = 2.7f;     // non-integer → never repeats
            compoundMix    = 0.5f;
            easing         = Easing::Bounce;
            phaseOffset    = 0.0f;
            break;
    }
}

// ---------------------------------------------------------------------------
// update() — runs the orbit, applies the hard safety clamp.
// ---------------------------------------------------------------------------

void AreaLightLayer::update(const RenderContext& ctx) {
    const Scene* scene = ctx.scene;
    glm::vec3 centroid = scene ? scene->centroid : glm::vec3(0.0f);

    safeRadius = computeSafeRadius(scene, safetyFactor);

    // 1. Evaluate the parametric orbit -> unit-length direction on the
    //    sphere around the centroid.
    glm::vec3 dir(0.0f, 0.0f, 1.0f);
    switch (orbitMode) {
        case OrbitMode::Circular:    dir = evalCircular_(ctx.elapsedSeconds);    break;
        case OrbitMode::Lissajous:   dir = evalLissajous_(ctx.elapsedSeconds);   break;
        case OrbitMode::Helix:       dir = evalHelix_(ctx.elapsedSeconds);       break;
        case OrbitMode::Random:      dir = evalRandom_(ctx.elapsedSeconds);      break;
        case OrbitMode::FigureEight: dir = evalFigureEight_(ctx.elapsedSeconds); break;
        case OrbitMode::Rose:        dir = evalRose_(ctx.elapsedSeconds);        break;
        case OrbitMode::Spiral:      dir = evalSpiral_(ctx.elapsedSeconds);      break;
        case OrbitMode::Pendulum:    dir = evalPendulum_(ctx.elapsedSeconds);    break;
        case OrbitMode::Compound:    dir = evalCompound_(ctx.elapsedSeconds);    break;
    }
    if (glm::length(dir) < 1e-5f) dir = glm::vec3(0.0f, 0.0f, 1.0f);
    dir = glm::normalize(dir);

    // 2. Hard safety clamp — the world-space radius is the larger of the
    //    operator's `orbitRadius` and the scene-derived `safeRadius`.
    //    Even if the operator drags `orbitRadius` to 0, we still sit on
    //    the safe sphere. The light cannot enter the AABB.
    float effective = std::max(orbitRadius, safeRadius);
    positionWorld   = centroid + dir * effective;

    // 3. Disc/rect orientation. If `faceCentroid` is true (the default),
    //    the panel always points at the centroid — i.e. illuminates the
    //    structure even as it orbits. Otherwise honour the operator's
    //    pan/tilt.
    if (faceCentroid) {
        glm::vec3 n = centroid - positionWorld;
        float len   = glm::length(n);
        normalWorld = (len > 1e-5f) ? (n / len) : glm::vec3(0.0f, 0.0f, -1.0f);
    } else {
        normalWorld = glm::normalize(panTiltToDir(panDeg, tiltDeg));
    }

    effectiveR = equivalentRadius();
}

// ---------------------------------------------------------------------------
// render() — like Beam/Directional, no direct GPU work. The StructureLayer
// collects us each frame and passes us into MetalRenderer::renderStructureMeshes.
// We still call update() here so the cached positionWorld/normalWorld are
// fresh by the time the structure pass packs uniforms (StructureLayer is
// guaranteed to render *after* light layers in the bus, but the collector
// runs inside StructureLayer::render so we can also be polled from there).
// ---------------------------------------------------------------------------

void AreaLightLayer::render(RenderContext& ctx) {
    if (state != LayerState::Enabled) return;
    update(ctx);
}

// ---------------------------------------------------------------------------
// ImGui inspector
// ---------------------------------------------------------------------------

void AreaLightLayer::drawInspector() {
    if (ImGui::CollapsingHeader("Shape", ImGuiTreeNodeFlags_DefaultOpen)) {
        const char* shapes[] = { "Disc", "Rectangle" };
        int si = static_cast<int>(shape);
        if (ImGui::Combo("Shape##area", &si, shapes, 2)) {
            shape = static_cast<Shape>(si);
        }
        if (shape == Shape::Disc) {
            ImGui::SliderFloat("Radius (m)##area", &radius, 0.05f, 8.0f);
        } else {
            ImGui::SliderFloat("Width (m)##area",  &width,  0.05f, 8.0f);
            ImGui::SliderFloat("Height (m)##area", &height, 0.05f, 8.0f);
        }
        ImGui::Checkbox("Face centroid (auto-aim panel)##area", &faceCentroid);
        if (!faceCentroid) {
            ImGui::SliderFloat("Pan (deg)##area",  &panDeg,  -180.0f, 180.0f);
            ImGui::SliderFloat("Tilt (deg)##area", &tiltDeg, -90.0f,  90.0f);
        } else {
            ImGui::TextDisabled("normal recomputed each frame: panel"
                                 " always looks at scene centroid");
        }
    }

    if (ImGui::CollapsingHeader("Light", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::ColorEdit3("Color##area", &color[0],
                          ImGuiColorEditFlags_PickerHueWheel
                          | ImGuiColorEditFlags_Float);
        ImGui::SliderFloat("Intensity##area", &intensity, 0.0f, 30.0f);
        ImGui::SliderFloat("Range (m)##area", &range,     0.5f, 200.0f);
    }

    if (ImGui::CollapsingHeader("Motion preset", ImGuiTreeNodeFlags_DefaultOpen)) {
        const char* presets[] = {
            "Custom", "Slow Orbit", "Figure-8 Sweep",
            "Rose Bloom", "Pendulum Pair", "Chaos Swarm"
        };
        int pi = static_cast<int>(preset);
        ImGui::SetNextItemWidth(170.0f);
        if (ImGui::Combo("Preset##area", &pi, presets, 6)) {
            applyPreset(static_cast<MotionPreset>(pi));
        }
        ImGui::TextDisabled("presets set mode + speed + amplitude + easing;");
        ImGui::TextDisabled("stagger 'Phase' across lights for a swarm.");
    }

    if (ImGui::CollapsingHeader("Orbit", ImGuiTreeNodeFlags_DefaultOpen)) {
        const char* modes[] = {
            "Circular", "Lissajous", "Helix", "Random",
            "Figure-8", "Rose", "Spiral", "Pendulum", "Compound"
        };
        int mi = static_cast<int>(orbitMode);
        ImGui::SetNextItemWidth(150.0f);
        if (ImGui::Combo("Mode##area", &mi, modes, 9)) {
            orbitMode = static_cast<OrbitMode>(mi);
            preset    = MotionPreset::Custom;   // hand-picked mode
        }

        // --- Articulated path: shared phase + easing -----------------------
        // Applies to every cyclic mode. Staggering `phaseOffset` across
        // several area lights forms the choreographed/articulated swarm.
        bool cyclic = (orbitMode != OrbitMode::Random);
        if (cyclic) {
            const char* eases[] = { "Linear", "Smoothstep", "Ease-in-out", "Bounce" };
            int ei = static_cast<int>(easing);
            ImGui::SetNextItemWidth(130.0f);
            if (ImGui::Combo("Easing##area", &ei, eases, 4)) {
                easing = static_cast<Easing>(ei);
                preset = MotionPreset::Custom;
            }
            if (ImGui::SliderFloat("Phase (turns)##area", &phaseOffset, 0.0f, 1.0f))
                preset = MotionPreset::Custom;
        }

        if (orbitMode == OrbitMode::Circular
         || orbitMode == OrbitMode::Helix) {
            const char* axes[] = { "X", "Y", "Z" };
            int ai = static_cast<int>(orbitAxis);
            ImGui::SetNextItemWidth(80.0f);
            if (ImGui::Combo("Axis##area", &ai, axes, 3)) {
                orbitAxis = static_cast<Axis>(ai);
            }
            ImGui::SliderFloat("Speed (deg/s)##area",
                                &speedDegPerSec, -180.0f, 180.0f);
        }

        if (orbitMode == OrbitMode::Lissajous) {
            const char* axes[] = { "X", "Y", "Z" };
            int ai = static_cast<int>(orbitAxis);
            ImGui::SetNextItemWidth(80.0f);
            if (ImGui::Combo("Pole axis##area", &ai, axes, 3)) {
                orbitAxis = static_cast<Axis>(ai);
            }
            ImGui::SliderFloat("Ax##area",  &lissAx, 0.0f, 1.5f);
            ImGui::SliderFloat("Ay##area",  &lissAy, 0.0f, 1.5f);
            ImGui::SliderFloat("a (freq x)##area", &lissA, 0.05f, 6.0f);
            ImGui::SliderFloat("b (freq y)##area", &lissB, 0.05f, 6.0f);
            ImGui::SliderFloat("phi (rad)##area", &lissPhi, 0.0f, kTau);
            ImGui::TextDisabled("a:b small-int ratios -> closed curves");
        }

        if (orbitMode == OrbitMode::Helix) {
            ImGui::SliderFloat("Bob freq (Hz)##area", &bobFreqHz, 0.0f, 4.0f);
            ImGui::SliderFloat("Bob amp (0..1)##area",  &bobAmp,   0.0f, 0.99f);
        }

        if (orbitMode == OrbitMode::Random) {
            ImGui::SliderFloat("Dwell (s)##area",  &dwellSeconds, 0.1f, 10.0f);
            ImGui::SliderFloat("Slerp ease##area", &slerpEase,    0.0f, 1.0f);
        }

        // --- Articulated path controls -----------------------------------
        if (orbitMode == OrbitMode::FigureEight
         || orbitMode == OrbitMode::Rose
         || orbitMode == OrbitMode::Spiral
         || orbitMode == OrbitMode::Pendulum
         || orbitMode == OrbitMode::Compound) {
            const char* axes[] = { "X", "Y", "Z" };
            int ai = static_cast<int>(orbitAxis);
            ImGui::SetNextItemWidth(80.0f);
            if (ImGui::Combo("Pole axis##areaArt", &ai, axes, 3)) {
                orbitAxis = static_cast<Axis>(ai);
            }
            if (ImGui::SliderFloat("Speed (deg/s)##areaArt",
                                   &speedDegPerSec, -180.0f, 180.0f))
                preset = MotionPreset::Custom;
        }

        if (orbitMode == OrbitMode::FigureEight) {
            ImGui::SliderFloat("Amplitude##areaFig", &pathAmplitude, 0.0f, 1.5f);
            ImGui::TextDisabled("Gerono lemniscate (crossing figure-8)");
        }

        if (orbitMode == OrbitMode::Rose) {
            ImGui::SliderFloat("Amplitude##areaRose", &pathAmplitude, 0.0f, 1.5f);
            ImGui::SliderInt("Petals k##areaRose", &roseK, 1, 12);
            ImGui::TextDisabled("r=cos(k.theta): odd k -> k petals, even -> 2k");
        }

        if (orbitMode == OrbitMode::Spiral) {
            ImGui::SliderFloat("Turns##areaSpiral",      &spiralTurns, 0.5f, 8.0f);
            ImGui::SliderFloat("Polar open##areaSpiral", &spiralPolar, 0.0f, 1.0f);
            ImGui::TextDisabled("azimuth sweeps while polar angle breathes");
        }

        if (orbitMode == OrbitMode::Pendulum) {
            ImGui::SliderFloat("Swing (deg)##areaPend", &pendSwingDeg, 5.0f, 170.0f);
            ImGui::SliderFloat("Damping##areaPend",     &pendDamping,  0.0f, 4.0f);
            ImGui::TextDisabled("damped harmonic rocking about the pole");
        }

        if (orbitMode == OrbitMode::Compound) {
            ImGui::SliderFloat("Amplitude##areaCmp",  &pathAmplitude, 0.0f, 1.5f);
            ImGui::SliderFloat("Harm. ratio##areaCmp", &compoundRatio, 0.5f, 8.0f);
            ImGui::SliderFloat("Harm. mix##areaCmp",   &compoundMix,   0.0f, 1.0f);
            ImGui::TextDisabled("non-integer ratio -> quasi-periodic weave");
        }
    }

    if (ImGui::CollapsingHeader("Orbit safety",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SliderFloat("Safety factor##area", &safetyFactor, 1.05f, 3.0f);
        ImGui::SliderFloat("Orbit radius (m)##area",
                            &orbitRadius, 0.0f, 50.0f);
        ImGui::TextDisabled("safeRadius (scene): %.2f m   |   effective: %.2f m",
                             safeRadius,
                             std::max(orbitRadius, safeRadius));
        ImGui::TextDisabled("(operator value clamped up to safeRadius "
                             "so the light never enters the mesh AABB)");
    }

    if (ImGui::CollapsingHeader("LFO modulation")) {
        ImGui::Checkbox("Intensity LFO##area", &intensityLFO.enabled);
        if (intensityLFO.enabled) {
            const char* waves[] = { "Sine", "Triangle", "Square", "Saw" };
            int w = static_cast<int>(intensityLFO.wave);
            ImGui::SetNextItemWidth(110.0f);
            if (ImGui::Combo("wave##areaLFO", &w, waves, 4)) {
                intensityLFO.wave = static_cast<LFO::Wave>(w);
            }
            ImGui::SliderFloat("amp##areaLFO",  &intensityLFO.amplitude, 0.0f, 20.0f);
            ImGui::SliderFloat("freq##areaLFO", &intensityLFO.freqHz,    0.0f, 8.0f);
            ImGui::SliderFloat("ph##areaLFO",   &intensityLFO.phase,     0.0f, 1.0f);
        }
    }

    if (ImGui::CollapsingHeader("Live readout")) {
        ImGui::TextDisabled("position:  (%.2f, %.2f, %.2f)",
                             positionWorld.x, positionWorld.y, positionWorld.z);
        ImGui::TextDisabled("normal:    (%.2f, %.2f, %.2f)",
                             normalWorld.x, normalWorld.y, normalWorld.z);
        ImGui::TextDisabled("eq.radius: %.3f m", effectiveR);
    }

    colorTag = color;
}

} // namespace spacegen
