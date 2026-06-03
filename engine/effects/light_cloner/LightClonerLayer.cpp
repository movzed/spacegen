#include "LightClonerLayer.h"
#include "ModulatorBank.h"
#include "Scene.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "imgui.h"

#include <algorithm>
#include <cmath>

namespace spacegen {

namespace {

constexpr float kPi          = 3.14159265358979323846f;
constexpr float kTwoPi       = 6.28318530717958647692f;
constexpr float kGoldenRatio = 1.61803398874989484820f;
// Golden angle in radians ≈ 2.39996322972865332 ≈ 137.5077640° per step.
constexpr float kGoldenAngle = kTwoPi - kTwoPi / kGoldenRatio;

constexpr int kMaxClones = 64;

// Build an orthonormal frame (right, up, axis) from `axis`. Both right and
// up are perpendicular to axis. The choice of `right` is arbitrary but
// deterministic — Gram-Schmidt against a fallback up vector.
void buildFrame(const glm::vec3& axisIn,
                glm::vec3& outAxis,
                glm::vec3& outRight,
                glm::vec3& outUp)
{
    outAxis = glm::length(axisIn) > 1e-5f ? glm::normalize(axisIn)
                                          : glm::vec3(0.0f, 0.0f, 1.0f);
    glm::vec3 ref = glm::vec3(0.0f, 0.0f, 1.0f);
    if (std::fabs(glm::dot(outAxis, ref)) > 0.99f) {
        ref = glm::vec3(0.0f, 1.0f, 0.0f);
    }
    outRight = glm::normalize(glm::cross(outAxis, ref));
    outUp    = glm::normalize(glm::cross(outRight, outAxis));
}

// RGB -> HSV. h in [0,1), s/v in [0,1].
glm::vec3 rgbToHsv(const glm::vec3& rgb) {
    float mx = std::max(rgb.r, std::max(rgb.g, rgb.b));
    float mn = std::min(rgb.r, std::min(rgb.g, rgb.b));
    float d  = mx - mn;
    float h = 0.0f, s = 0.0f, v = mx;
    if (d > 1e-6f) {
        s = d / std::max(1e-6f, mx);
        if      (mx == rgb.r) h = (rgb.g - rgb.b) / d + (rgb.g < rgb.b ? 6.0f : 0.0f);
        else if (mx == rgb.g) h = (rgb.b - rgb.r) / d + 2.0f;
        else                  h = (rgb.r - rgb.g) / d + 4.0f;
        h /= 6.0f;
    }
    return glm::vec3(h, s, v);
}

// HSV -> RGB. h in [0,1) wrap, s/v in [0,1].
glm::vec3 hsvToRgb(float h, float s, float v) {
    h = h - std::floor(h);
    float i = std::floor(h * 6.0f);
    float f = h * 6.0f - i;
    float p = v * (1.0f - s);
    float q = v * (1.0f - f * s);
    float t = v * (1.0f - (1.0f - f) * s);
    int   k = static_cast<int>(i) % 6;
    switch (k) {
        case 0: return glm::vec3(v, t, p);
        case 1: return glm::vec3(q, v, p);
        case 2: return glm::vec3(p, v, t);
        case 3: return glm::vec3(p, q, v);
        case 4: return glm::vec3(t, p, v);
        case 5: default: return glm::vec3(v, p, q);
    }
}

// Cheap deterministic 3-component hash: returns three floats in (-1, +1].
glm::vec3 hash3(uint32_t seed, int i) {
    // sin-based fractional hash. Not cryptographic; we want determinism +
    // visual decorrelation. Same trick used in MetalRenderer's vsHash.
    float fi = static_cast<float>(i);
    float fs = static_cast<float>(seed & 0xFFFFFu) * 0.001f;
    auto frac = [](float x) { return x - std::floor(x); };
    float hx = frac(std::sin(fi * 12.9898f + fs * 78.233f) * 43758.5453f) * 2.0f - 1.0f;
    float hy = frac(std::sin(fi * 39.346f  + fs * 11.135f) * 24634.6345f) * 2.0f - 1.0f;
    float hz = frac(std::sin(fi * 73.156f  + fs * 45.213f) * 53758.5453f) * 2.0f - 1.0f;
    return glm::vec3(hx, hy, hz);
}

// Pan/tilt -> direction, relative to a baseForward axis. Same math as
// BeamLayer::panTiltToDir; duplicated here so this TU is self-contained.
glm::vec3 panTiltToDir(float panDeg, float tiltDeg, const glm::vec3& baseFwd) {
    glm::vec3 worldUp(0.0f, 0.0f, 1.0f);
    glm::vec3 f = glm::normalize(baseFwd);
    glm::vec3 ref = (std::fabs(glm::dot(f, worldUp)) > 0.99f)
                    ? glm::vec3(0.0f, 1.0f, 0.0f) : worldUp;
    glm::vec3 r = glm::normalize(glm::cross(f, ref));
    glm::vec3 u = glm::normalize(glm::cross(r, f));

    float pr = panDeg  * kPi / 180.0f;
    float tr = tiltDeg * kPi / 180.0f;
    glm::quat qPan  = glm::angleAxis(pr, u);
    glm::vec3 fPan  = qPan * f;
    glm::vec3 rPan  = qPan * r;
    glm::quat qTilt = glm::angleAxis(tr, rPan);
    return glm::normalize(qTilt * fPan);
}

void drawLFOWidget(const char* label, LFO& lfo, float ampMax) {
    ImGui::PushID(label);
    ImGui::Checkbox("##en", &lfo.enabled);
    ImGui::SameLine();
    ImGui::TextDisabled("%s", label);
    if (lfo.enabled) {
        ImGui::Indent(16.0f);
        const char* waves[] = { "Sine", "Triangle", "Square", "Saw" };
        int w = static_cast<int>(lfo.wave);
        ImGui::SetNextItemWidth(110.0f);
        if (ImGui::Combo("wave", &w, waves, 4)) {
            lfo.wave = static_cast<LFO::Wave>(w);
        }
        ImGui::SliderFloat("amp",  &lfo.amplitude, 0.0f, ampMax);
        ImGui::SliderFloat("freq", &lfo.freqHz,    0.0f, 8.0f);
        ImGui::SliderFloat("ph",   &lfo.phase,     0.0f, 1.0f);
        ImGui::Unindent(16.0f);
    }
    ImGui::PopID();
}

} // anon

LightClonerLayer::LightClonerLayer() {
    name      = "Cloner";
    blendMode = BlendMode::Add;
    colorTag  = glm::vec3(0.85f, 0.55f, 0.95f);
}

glm::vec3 LightClonerLayer::effectiveOrigin(const RenderContext& ctx) const {
    if (followCamera) return ctx.cameraWorldPos;
    if (useSceneCentroid && ctx.scene) {
        // Scene::centroid is the loaded-mesh AABB center; fall back to
        // the configured origin if the scene has no meshes loaded.
        glm::vec3 c = ctx.scene->centroid;
        if (std::isfinite(c.x) && std::isfinite(c.y) && std::isfinite(c.z)) {
            return c;
        }
    }
    return origin;
}

std::vector<glm::vec3>
LightClonerLayer::clonePositions(const glm::vec3& centroid) const
{
    const int N = std::max(1, std::min(clones, kMaxClones));
    std::vector<glm::vec3> out;
    out.reserve(N);

    glm::vec3 ax, right, up;
    buildFrame(axis, ax, right, up);

    switch (pattern) {
    case Pattern::Ring: {
        for (int i = 0; i < N; ++i) {
            float theta = kTwoPi * static_cast<float>(i) / static_cast<float>(N);
            out.push_back(centroid
                          + radius * (std::cos(theta) * right
                                    + std::sin(theta) * up));
        }
        break;
    }

    case Pattern::Helix: {
        const float denom = (N > 1) ? static_cast<float>(N - 1) : 1.0f;
        for (int i = 0; i < N; ++i) {
            float u01   = static_cast<float>(i) / denom;          // 0..1
            float theta = kTwoPi * turns * u01;
            float h     = stepHeight * (static_cast<float>(i)
                                       - 0.5f * static_cast<float>(N - 1));
            out.push_back(centroid
                          + radius * (std::cos(theta) * right
                                    + std::sin(theta) * up)
                          + ax * h);
        }
        break;
    }

    case Pattern::Grid: {
        int cols = std::max(1, gridCols);
        int rows = (N + cols - 1) / cols;
        for (int i = 0; i < N; ++i) {
            int col = i % cols;
            int row = i / cols;
            float cx = (static_cast<float>(col) - 0.5f * static_cast<float>(cols - 1))
                       * gridSpacing;
            float cy = (static_cast<float>(row) - 0.5f * static_cast<float>(rows - 1))
                       * gridSpacing;
            out.push_back(centroid + cx * right + cy * up);
        }
        break;
    }

    case Pattern::FibSphere: {
        // Saff-Kuijlaars / Marques: z varies linearly from +1 to -1, with
        // a +0.5 offset to push samples away from the poles, and the
        // azimuth advances by the golden angle per index.
        const float Nf = static_cast<float>(N);
        for (int i = 0; i < N; ++i) {
            float z = 1.0f - 2.0f * (static_cast<float>(i) + 0.5f) / Nf;
            float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
            float th = kGoldenAngle * static_cast<float>(i);
            glm::vec3 unit(r * std::cos(th), r * std::sin(th), z);
            out.push_back(centroid
                          + radius * (unit.x * right
                                    + unit.y * up
                                    + unit.z * ax));
        }
        break;
    }

    case Pattern::Random: {
        for (int i = 0; i < N; ++i) {
            glm::vec3 h = hash3(seed, i);
            out.push_back(centroid + randomRadius
                          * (h.x * right + h.y * up + h.z * ax));
        }
        break;
    }
    }

    return out;
}

glm::vec3 LightClonerLayer::cloneColor(int idx, int total) const {
    const float t = (total > 1)
        ? static_cast<float>(idx) / static_cast<float>(total - 1)
        : 0.0f;
    switch (colorScheme) {
    case ColorScheme::Uniform:
        return templateColor;

    case ColorScheme::HueChase: {
        glm::vec3 hsv = rgbToHsv(colorStart);
        float frac = (total > 0)
            ? static_cast<float>(idx) / static_cast<float>(total)
            : 0.0f;
        float h = hsv.x + frac * hueSpread;
        return hsvToRgb(h, hsv.y, hsv.z);
    }

    case ColorScheme::Gradient:
        return glm::mix(colorStart, colorEnd, t);

    case ColorScheme::Triadic: {
        glm::vec3 hsv = rgbToHsv(colorStart);
        float h = hsv.x + static_cast<float>(idx % 3) * (1.0f / 3.0f);
        return hsvToRgb(h, hsv.y, hsv.z);
    }

    case ColorScheme::Complementary: {
        glm::vec3 hsv = rgbToHsv(colorStart);
        float h = hsv.x + static_cast<float>(idx % 2) * 0.5f;
        return hsvToRgb(h, hsv.y, hsv.z);
    }
    }
    return templateColor;
}

glm::vec3 LightClonerLayer::cloneDirection(
    double t, int idx, int total,
    const glm::vec3& centroid, const glm::vec3& clonePos,
    const ModulatorBank* mods) const
{
    // Per-clone phase (cycles).
    const float ph = (total > 1 && phaseSpread > 0.0f)
        ? (static_cast<float>(idx) / static_cast<float>(total)) * phaseSpread
        : 0.0f;

    // Base aim: radial from centroid (outward), or inward.
    glm::vec3 radial = clonePos - centroid;
    if (glm::length(radial) < 1e-4f) {
        // Degenerate (clone at centroid). Fall back to +Y so panTiltToDir
        // has a sane frame.
        radial = glm::vec3(0.0f, 1.0f, 0.0f);
    }
    glm::vec3 baseFwd = aimInward ? -glm::normalize(radial)
                                   :  glm::normalize(radial);

    // Apply LFO + motion offsets on top of pan/tilt.
    auto motion = motionLFO.eval(t, ph);
    float p  = panDeg  + panLFO.eval(t, ph)  + motion.panDeg;
    float ti = tiltDeg + tiltLFO.eval(t, ph) + motion.tiltDeg;
    (void)mods;  // reserved for future modulator-bank bindings on the cloner
    return panTiltToDir(p, ti, baseFwd);
}

glm::vec3 LightClonerLayer::swarmOffset(double t, int idx, int total) const {
    if (!swarmEnabled || swarmAmp <= 0.0f) return glm::vec3(0.0f);
    const float ph2pi = (total > 1)
        ? (static_cast<float>(idx) / static_cast<float>(total))
          * phaseSpread * kTwoPi
        : 0.0f;
    const double wx = static_cast<double>(swarmFreq) * kTwoPi;
    const double wy = wx * static_cast<double>(swarmRatioY);
    const double wz = wx * static_cast<double>(swarmRatioZ);
    glm::vec3 ax, right, up;
    buildFrame(axis, ax, right, up);
    float dx = swarmAmp        * static_cast<float>(std::sin(wx * t + ph2pi));
    float dy = swarmAmp        * static_cast<float>(std::sin(wy * t + ph2pi + 1.7));
    float dz = swarmAmp * 0.4f * static_cast<float>(std::sin(wz * t + ph2pi + 3.1));
    return dx * right + dy * up + dz * ax;
}

float LightClonerLayer::effectorWeight(const Effector& e, const glm::vec3& p) {
    switch (e.falloff) {
    case Effector::Falloff::None:
        return 1.0f;

    case Effector::Falloff::Sphere: {
        const float r = std::max(1e-4f, e.falloffRadius);
        float d = glm::length(p - e.falloffCenter) / r;       // 0 at centre
        d = std::clamp(d, 0.0f, 1.0f);
        // smoothstep-style ease so the field edge isn't a hard ring.
        float w = 1.0f - d;
        return w * w * (3.0f - 2.0f * w);
    }

    case Effector::Falloff::Box: {
        const float r = std::max(1e-4f, e.falloffRadius);
        glm::vec3 d = glm::abs(p - e.falloffCenter) / r;      // per-axis 0..1+
        float m = std::clamp(std::max(d.x, std::max(d.y, d.z)), 0.0f, 1.0f);
        float w = 1.0f - m;
        return w * w * (3.0f - 2.0f * w);
    }
    }
    return 1.0f;
}

namespace {

// Per-clone modulation scalar in [-1, 1] for a given effector type. Plain is
// uniform (+1) so its offsets apply at full strength across the field; the
// others decorrelate / ramp / animate.
float effectorModulation(const LightClonerLayer::Effector& e,
                          int idx, int total, double t) {
    using Type = LightClonerLayer::Effector::Type;
    switch (e.type) {
    case Type::Plain:
        return 1.0f;

    case Type::Random: {
        // hash3's x-channel is already in (-1, 1]; offset the index so two
        // Random effectors don't share the same sequence.
        return hash3(0x9E3779B9u, idx * 3 + 1).x;
    }

    case Type::Step: {
        // Ramp 0..1 across the clone count, remapped to [-1, 1] so the
        // effector pushes symmetrically around the template.
        float s = (total > 1)
            ? static_cast<float>(idx) / static_cast<float>(total - 1)
            : 0.0f;
        return s * 2.0f - 1.0f;
    }

    case Type::Sine: {
        float arg = kTwoPi * (e.freqHz * static_cast<float>(t)
                              + e.phase
                              + static_cast<float>(idx) * e.spread);
        return std::sin(arg);
    }
    }
    return 0.0f;
}

} // anon

int LightClonerLayer::expandSpots(const RenderContext& ctx,
                                    std::vector<VirtualSpot>& sink) const
{
    if (state != LayerState::Enabled) return 0;
    if (opacity <= 0.0f)               return 0;
    if (lightKind != LightKind::Spot)  return 0;
    const int N = std::max(1, std::min(clones, kMaxClones));

    const glm::vec3 centroid = effectiveOrigin(ctx);
    auto positions = clonePositions(centroid);

    // Pre-compute the cone cosines (shared across clones).
    float innerRad = templateInnerDeg * kPi / 180.0f;
    float outerRad = templateOuterDeg * kPi / 180.0f;
    if (outerRad < innerRad) outerRad = innerRad;
    const float innerCos = std::cos(innerRad);
    const float outerCos = std::cos(outerRad);

    const double t = ctx.elapsedSeconds;
    const ModulatorBank* mods = ctx.scene ? &ctx.scene->modulators : nullptr;

    int emitted = 0;
    for (int i = 0; i < N; ++i) {
        // --- Accumulate the effector stack for this clone --------------
        // Effectors are weighted by their falloff field evaluated at the
        // clone's BASE position (so a clone moving out of a field doesn't
        // chase its own weight). Contributions stack additively in order.
        glm::vec3 effPos   = glm::vec3(0.0f);
        float     effInten = 0.0f;
        glm::vec3 effColor = glm::vec3(0.0f);
        float     effCone  = 0.0f;       // degrees on the outer cone
        for (const Effector& e : effectors) {
            if (!e.enabled) continue;
            float w = effectorWeight(e, positions[i]);
            if (w <= 0.0f) continue;
            float m  = effectorModulation(e, i, N, t);
            float wm = w * m;
            effPos   += e.posOffset       * wm;
            effInten += e.intensityOffset * wm;
            effColor += e.colorOffset     * wm;
            effCone  += e.coneOffset      * wm;
        }

        VirtualSpot vs;
        // Position: base + swarm + effector offset. Aim is recomputed from
        // the MOVED position so the clone keeps pointing at/away from the
        // centroid (projection-mapping hard rule: effectors offset fixtures
        // but never aim them blindly into empty air).
        vs.worldPos  = positions[i] + swarmOffset(t, i, N) + effPos;
        vs.direction = cloneDirection(t, i, N, centroid, vs.worldPos, mods);
        vs.color     = glm::max(cloneColor(i, N) + effColor, glm::vec3(0.0f));

        // Per-clone phase also shifts intensity LFO + motion intensity.
        const float ph = (N > 1)
            ? (static_cast<float>(i) / static_cast<float>(N)) * phaseSpread
            : 0.0f;
        auto motion = motionLFO.eval(t, ph);
        float inten = templateIntensity
                    + intensityLFO.eval(t, ph)
                    + motion.intensity
                    + effInten;
        vs.intensity = std::max(0.0f, inten) * opacity;
        vs.range     = templateRange;

        // Cone: nudge the OUTER half-angle, keep inner <= outer and both in
        // a valid (0, 89]deg range so cos() stays monotonic / well-formed.
        if (effCone != 0.0f) {
            float outerDeg = std::clamp(templateOuterDeg + effCone, 0.5f, 89.0f);
            float innerDeg = std::min(templateInnerDeg, outerDeg);
            vs.innerCos = std::cos(innerDeg * kPi / 180.0f);
            vs.outerCos = std::cos(outerDeg * kPi / 180.0f);
        } else {
            vs.innerCos = innerCos;
            vs.outerCos = outerCos;
        }
        sink.push_back(vs);
        ++emitted;
    }
    return emitted;
}

// ---- Inspector --------------------------------------------------------

void LightClonerLayer::drawInspector() {
    // -- Light kind ----
    const char* kinds[] = { "Spot", "Area (gated)" };
    int ki = static_cast<int>(lightKind);
    if (ImGui::Combo("Light kind##cloner", &ki, kinds, 2)) {
        lightKind = static_cast<LightKind>(ki);
    }
    if (lightKind == LightKind::Area) {
        ImGui::TextDisabled("Area light expansion requires AreaLightLayer");
        ImGui::TextDisabled("(landing in a sibling task).");
    }

    // -- Count + pattern ----
    ImGui::Separator();
    ImGui::TextDisabled("DISTRIBUTION");
    ImGui::SliderInt("Clones##cloner", &clones, 1, kMaxClones);
    const char* patterns[] = { "Ring", "Helix", "Grid",
                               "Fibonacci sphere", "Random" };
    int pi = static_cast<int>(pattern);
    if (ImGui::Combo("Pattern##cloner", &pi, patterns, 5)) {
        pattern = static_cast<Pattern>(pi);
    }

    // Origin / axis ----
    ImGui::Checkbox("Follow camera##cloner", &followCamera);
    if (!followCamera) {
        ImGui::Checkbox("Use scene centroid##cloner", &useSceneCentroid);
        if (!useSceneCentroid) {
            ImGui::DragFloat3("Origin##cloner", &origin[0], 0.05f, -50.0f, 50.0f);
        }
    }
    ImGui::DragFloat3("Axis##cloner", &axis[0], 0.02f, -1.0f, 1.0f);

    // Pattern-specific ----
    switch (pattern) {
    case Pattern::Ring:
        ImGui::SliderFloat("Radius (m)##cloner", &radius, 0.1f, 30.0f);
        break;
    case Pattern::Helix:
        ImGui::SliderFloat("Radius (m)##cloner",     &radius,     0.1f, 30.0f);
        ImGui::SliderFloat("Step height (m)##cloner",&stepHeight, 0.0f, 5.0f);
        ImGui::SliderFloat("Turns##cloner",          &turns,      0.1f, 10.0f);
        break;
    case Pattern::Grid:
        ImGui::SliderInt  ("Columns##cloner",   &gridCols,    1, 16);
        ImGui::SliderFloat("Cell size (m)##cl", &gridSpacing, 0.1f, 5.0f);
        break;
    case Pattern::FibSphere:
        ImGui::SliderFloat("Sphere radius (m)##cl", &radius, 0.1f, 30.0f);
        ImGui::TextDisabled("Golden-angle spiral (Saff-Kuijlaars 1997)");
        break;
    case Pattern::Random: {
        ImGui::SliderFloat("Box half-extent (m)##cl", &randomRadius, 0.1f, 30.0f);
        int s = static_cast<int>(seed);
        if (ImGui::InputInt("Seed##cloner", &s, 1, 100)) {
            seed = static_cast<uint32_t>(s);
        }
        break;
    }
    }

    // Phase distribution ----
    ImGui::SliderFloat("Phase spread##cloner", &phaseSpread, 0.0f, 1.0f);
    ImGui::TextDisabled("0 = synced | 1 = full chase across N clones");

    // -- Color ----
    ImGui::Separator();
    ImGui::TextDisabled("COLOR");
    const char* schemes[] = {
        "Uniform", "Hue chase", "Gradient",
        "Triadic", "Complementary"
    };
    int csi = static_cast<int>(colorScheme);
    if (ImGui::Combo("Scheme##cloner", &csi, schemes, 5)) {
        colorScheme = static_cast<ColorScheme>(csi);
    }
    ImGui::ColorEdit3("Start color##cloner", &colorStart[0],
                       ImGuiColorEditFlags_PickerHueWheel
                       | ImGuiColorEditFlags_Float);
    if (colorScheme == ColorScheme::Gradient) {
        ImGui::ColorEdit3("End color##cloner", &colorEnd[0],
                          ImGuiColorEditFlags_PickerHueWheel
                          | ImGuiColorEditFlags_Float);
    }
    if (colorScheme == ColorScheme::HueChase) {
        ImGui::SliderFloat("Hue spread##cloner", &hueSpread, 0.0f, 1.0f);
    }
    if (colorScheme == ColorScheme::Uniform) {
        ImGui::ColorEdit3("Template color##cloner", &templateColor[0],
                          ImGuiColorEditFlags_PickerHueWheel
                          | ImGuiColorEditFlags_Float);
    }

    // -- Template light ----
    if (ImGui::CollapsingHeader("Template light",
                                 ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Aim inward##cloner", &aimInward);
        ImGui::TextDisabled("OFF: clones look outward from centroid");
        ImGui::TextDisabled("ON:  clones look inward at centroid");
        ImGui::SliderFloat("Pan (deg)##cloner",  &panDeg,  -180.0f, 180.0f);
        ImGui::SliderFloat("Tilt (deg)##cloner", &tiltDeg, -90.0f,  90.0f);
        ImGui::SliderFloat("Intensity##cloner",  &templateIntensity, 0.0f, 30.0f);
        ImGui::SliderFloat("Range (m)##cloner",  &templateRange,     0.5f, 200.0f);
        ImGui::SliderFloat("Inner cone (deg)##cloner", &templateInnerDeg, 0.5f, 60.0f);
        ImGui::SliderFloat("Outer cone (deg)##cloner", &templateOuterDeg, 0.5f, 60.0f);
        if (templateOuterDeg < templateInnerDeg) templateOuterDeg = templateInnerDeg;
    }

    if (ImGui::CollapsingHeader("Per-axis LFO modulation")) {
        drawLFOWidget("pan",       panLFO,       180.0f);
        drawLFOWidget("tilt",      tiltLFO,       90.0f);
        drawLFOWidget("intensity", intensityLFO,  20.0f);
    }

    if (ImGui::CollapsingHeader("Motion pattern (3-axis)")) {
        const char* mpatterns[] = {
            "Off", "Circle", "Figure-8", "Ballyhoo", "Wave",
            "Sweep", "Can-can", "Strobe + pan"
        };
        int mp = static_cast<int>(motionLFO.pattern);
        ImGui::SetNextItemWidth(170.0f);
        if (ImGui::Combo("Pattern##cloner-motion", &mp, mpatterns, 8)) {
            motionLFO.pattern = static_cast<MotionLFO::Pattern>(mp);
        }
        if (motionLFO.pattern != MotionLFO::Pattern::Off) {
            ImGui::Indent(8.0f);
            ImGui::SliderFloat("Freq (Hz)##cm", &motionLFO.freqHz, 0.0f, 4.0f);
            ImGui::SliderFloat("Pan amp##cm",   &motionLFO.panAmp,  0.0f, 180.0f);
            ImGui::SliderFloat("Tilt amp##cm",  &motionLFO.tiltAmp, 0.0f, 90.0f);
            ImGui::SliderFloat("Int amp##cm",   &motionLFO.intAmp,  0.0f, 20.0f);
            ImGui::SliderFloat("Phase##cm",     &motionLFO.phase,   0.0f, 1.0f);
            ImGui::Unindent(8.0f);
        }
    }

    if (ImGui::CollapsingHeader("Swarm motion (per-clone Lissajous)")) {
        ImGui::Checkbox("Enabled##swarm",        &swarmEnabled);
        if (swarmEnabled) {
            ImGui::SliderFloat("Amplitude (m)##swarm", &swarmAmp,    0.0f, 3.0f);
            ImGui::SliderFloat("Frequency (Hz)##swarm",&swarmFreq,   0.0f, 2.0f);
            ImGui::SliderFloat("Ratio Y##swarm",       &swarmRatioY, 0.1f, 3.0f);
            ImGui::SliderFloat("Ratio Z##swarm",       &swarmRatioZ, 0.1f, 3.0f);
            ImGui::TextDisabled("Irrational ratios = non-repeating orbit");
        }
    }

    // -- Effector stack (Notch-style) ----
    if (ImGui::CollapsingHeader("Effectors (falloff-weighted)",
                                 ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextDisabled("Per-clone modifiers weighted by a spatial field.");
        ImGui::TextDisabled("Stack additively; empty = no change.");

        if (ImGui::Button("Add effector##cloner")) {
            effectors.emplace_back();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(%d)", static_cast<int>(effectors.size()));

        const char* effTypes[]    = { "Plain", "Random", "Step", "Sine" };
        const char* effFalloffs[] = { "None", "Sphere", "Box" };

        int removeIdx = -1;
        for (int ei = 0; ei < static_cast<int>(effectors.size()); ++ei) {
            Effector& e = effectors[static_cast<size_t>(ei)];
            ImGui::PushID(ei);
            ImGui::Separator();

            ImGui::Checkbox("##effen", &e.enabled);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(110.0f);
            int et = static_cast<int>(e.type);
            if (ImGui::Combo("type", &et, effTypes, 4)) {
                e.type = static_cast<Effector::Type>(et);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("X")) {
                removeIdx = ei;
            }

            if (e.enabled) {
                ImGui::Indent(12.0f);

                // Falloff field.
                ImGui::SetNextItemWidth(110.0f);
                int ef = static_cast<int>(e.falloff);
                if (ImGui::Combo("falloff", &ef, effFalloffs, 3)) {
                    e.falloff = static_cast<Effector::Falloff>(ef);
                }
                if (e.falloff != Effector::Falloff::None) {
                    ImGui::DragFloat3("center", &e.falloffCenter[0],
                                      0.05f, -50.0f, 50.0f);
                    ImGui::SliderFloat("radius (m)", &e.falloffRadius,
                                       0.1f, 30.0f);
                } else {
                    ImGui::TextDisabled("(full strength everywhere)");
                }

                // Per-channel offsets.
                ImGui::DragFloat3("pos offset (m)", &e.posOffset[0],
                                  0.02f, -10.0f, 10.0f);
                ImGui::SliderFloat("intensity off", &e.intensityOffset,
                                   -20.0f, 20.0f);
                ImGui::DragFloat3("color offset", &e.colorOffset[0],
                                  0.01f, -1.0f, 1.0f);
                ImGui::SliderFloat("cone off (deg)", &e.coneOffset,
                                   -45.0f, 45.0f);

                if (e.type == Effector::Type::Sine) {
                    ImGui::SliderFloat("freq (Hz)", &e.freqHz, 0.0f, 4.0f);
                    ImGui::SliderFloat("phase",     &e.phase,  0.0f, 1.0f);
                    ImGui::SliderFloat("spread",    &e.spread, 0.0f, 1.0f);
                }

                ImGui::Unindent(12.0f);
            }
            ImGui::PopID();
        }
        if (removeIdx >= 0) {
            effectors.erase(effectors.begin() + removeIdx);
        }
    }

    // Layer color tag mirrors start color so the rack row reads at a glance.
    colorTag = colorStart;
}

} // namespace spacegen
