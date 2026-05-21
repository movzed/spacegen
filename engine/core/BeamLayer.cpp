#include "BeamLayer.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "imgui.h"

#include <cmath>

namespace spacegen {

namespace {

constexpr float kPi = 3.14159265358979323846f;

// Pan/tilt -> direction, relative to a baseForward axis (world +Y or
// camera forward). Pan rotates around the world up axis (Z); tilt then
// rotates around the resulting local right axis (so tilt is always "up
// from the panned forward").
glm::vec3 panTiltToDir(float panDeg, float tiltDeg, const glm::vec3& baseForward) {
    glm::vec3 worldUp = glm::vec3(0.0f, 0.0f, 1.0f);
    // Build an orthonormal frame from baseForward.
    glm::vec3 f = glm::normalize(baseForward);
    // If baseForward is too close to worldUp, fallback up = +Y.
    glm::vec3 ref = (std::fabs(glm::dot(f, worldUp)) > 0.99f)
                    ? glm::vec3(0.0f, 1.0f, 0.0f) : worldUp;
    glm::vec3 r = glm::normalize(glm::cross(f, ref));   // local right
    glm::vec3 u = glm::normalize(glm::cross(r, f));     // local up

    float pr = panDeg  * kPi / 180.0f;
    float tr = tiltDeg * kPi / 180.0f;
    // Pan around u (yaw):
    glm::quat qPan  = glm::angleAxis(pr, u);
    glm::vec3 fPan  = qPan * f;
    glm::vec3 rPan  = qPan * r;
    // Tilt around the panned right axis (pitch):
    glm::quat qTilt = glm::angleAxis(tr, rPan);
    glm::vec3 dir   = qTilt * fPan;
    return glm::normalize(dir);
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

BeamLayer::BeamLayer() {
    name      = "Spot";
    blendMode = BlendMode::Add;
    colorTag  = glm::vec3(1.0f, 0.95f, 0.85f);
}

glm::vec3 BeamLayer::directionAtTime(double t,
                                       const glm::vec3& baseForward) const
{
    auto motion = motionLFO.eval(t);
    float p  = panDeg  + panLFO.eval(t)  + motion.panDeg;
    float ti = tiltDeg + tiltLFO.eval(t) + motion.tiltDeg;
    return panTiltToDir(p, ti, baseForward);
}

float BeamLayer::intensityAtTime(double t) const {
    auto motion = motionLFO.eval(t);
    return std::max(0.0f, intensity + intensityLFO.eval(t) + motion.intensity);
}

std::vector<glm::vec3>
BeamLayer::fixturePositions(const RenderContext& ctx) const {
    int N = std::max(1, std::min(fixtureCount, 8));
    std::vector<glm::vec3> result;
    result.reserve(N);

    glm::vec3 center = followCamera ? ctx.cameraWorldPos : origin;

    if (layout == Layout::Single || N == 1) {
        result.push_back(center);
        return result;
    }

    glm::vec3 fwd = followCamera
        ? glm::normalize(ctx.cameraForward)
        : glm::vec3(0.0f, 1.0f, 0.0f);
    glm::vec3 worldUp(0.0f, 0.0f, 1.0f);
    if (std::fabs(glm::dot(fwd, worldUp)) > 0.99f) {
        worldUp = glm::vec3(0.0f, 1.0f, 0.0f);
    }
    glm::vec3 right = glm::normalize(glm::cross(fwd, worldUp));

    if (layout == Layout::Linear) {
        for (int i = 0; i < N; ++i) {
            float t = (static_cast<float>(i) - (N - 1) * 0.5f) * spacing;
            result.push_back(center + right * t);
        }
    } else if (layout == Layout::Arc) {
        for (int i = 0; i < N; ++i) {
            float u = (N > 1) ? static_cast<float>(i) / (N - 1) : 0.5f;
            float angleDeg = -arcSpreadDeg * 0.5f + arcSpreadDeg * u;
            float a = angleDeg * 3.14159265358979323846f / 180.0f;
            float ca = std::cos(a), sa = std::sin(a);
            glm::vec3 dir = fwd * ca + right * sa;
            result.push_back(center + dir * arcRadius);
        }
    }

    return result;
}

// Per-fixture phase shift (cycles, 0..1) so multiple fixtures stagger across
// the LFO cycle when fixturePhase > 0.
static float fixturePhaseShift(int idx, int total, float fixturePhase01) {
    if (total <= 1 || fixturePhase01 == 0.0f) return 0.0f;
    return (static_cast<float>(idx) / static_cast<float>(total))
           * fixturePhase01;
}

glm::vec3 BeamLayer::colorForFixture(int idx) const {
    if (!useFixtureColors || fixtureColors.empty()) return color;
    int n = static_cast<int>(fixtureColors.size());
    return fixtureColors[((idx % n) + n) % n];
}

glm::vec3 BeamLayer::directionAtTimeForFixture(
    double t, int idx, int total, const glm::vec3& baseForward) const
{
    float ph     = fixturePhaseShift(idx, total, fixturePhase);
    auto motion  = motionLFO.eval(t, ph);
    float p      = panDeg  + panLFO.eval(t,  ph) + motion.panDeg;
    float ti     = tiltDeg + tiltLFO.eval(t, ph) + motion.tiltDeg;
    return panTiltToDir(p, ti, baseForward);
}

float BeamLayer::intensityAtTimeForFixture(double t, int idx, int total) const
{
    float ph    = fixturePhaseShift(idx, total, fixturePhase);
    auto motion = motionLFO.eval(t, ph);
    return std::max(0.0f, intensity + intensityLFO.eval(t, ph)
                          + motion.intensity);
}

void BeamLayer::drawInspector() {
    ImGui::Checkbox("Follow camera##spot", &followCamera);
    if (followCamera) {
        ImGui::TextDisabled("origin tracks the scene camera");
    } else {
        ImGui::DragFloat3("Origin##spot", &origin[0], 0.05f, -20.0f, 20.0f);
    }

    // ---- RIG LAYOUT (multi-fixture stack, symmetric around center) ----
    ImGui::Separator();
    ImGui::TextDisabled("RIG (max 4 per side)");
    struct Preset {
        const char* name;
        Layout layout;
        int    count;
        float  spacing;
        float  arcSpread;
        float  arcRadius;
    };
    static const Preset kPresets[] = {
        // name         layout          count  spacing  arcSpread  arcRadius
        {"Single",      Layout::Single, 1,     0.0f,    0.0f,      0.0f },
        {"Pair L/R",    Layout::Linear, 2,     2.0f,    0.0f,      0.0f },
        {"Trio",        Layout::Linear, 3,     1.5f,    0.0f,      0.0f },
        {"Quad row",    Layout::Linear, 4,     1.0f,    0.0f,      0.0f },
        {"6-truss",     Layout::Linear, 6,     0.8f,    0.0f,      0.0f },
        {"8-truss",     Layout::Linear, 8,     0.6f,    0.0f,      0.0f },
        {"Arc 5 (90°)", Layout::Arc,    5,     0.0f,    90.0f,     4.0f },
        {"Arc 7 (120°)",Layout::Arc,    7,     0.0f,    120.0f,    5.0f },
        {"Custom",      Layout::Single, 1,     0.0f,    0.0f,      0.0f },
    };
    static const char* kPresetNames[] = {
        "Single", "Pair L/R", "Trio", "Quad row", "6-truss",
        "8-truss", "Arc 5 (90°)", "Arc 7 (120°)", "Custom"
    };
    static int activePreset = 0;
    if (ImGui::Combo("Preset##rig", &activePreset, kPresetNames,
                      IM_ARRAYSIZE(kPresetNames))) {
        if (activePreset < IM_ARRAYSIZE(kPresetNames) - 1) { // not "Custom"
            const auto& p = kPresets[activePreset];
            layout       = p.layout;
            fixtureCount = p.count;
            if (p.spacing   > 0.0f) spacing      = p.spacing;
            if (p.arcSpread > 0.0f) arcSpreadDeg = p.arcSpread;
            if (p.arcRadius > 0.0f) arcRadius    = p.arcRadius;
        }
    }
    const char* layoutNames[] = { "Single", "Linear", "Arc" };
    int li = static_cast<int>(layout);
    if (ImGui::Combo("Layout##rig", &li, layoutNames, 3)) {
        layout = static_cast<Layout>(li);
    }
    ImGui::SliderInt("Fixtures##rig", &fixtureCount, 1, 8);
    if (layout == Layout::Linear) {
        ImGui::SliderFloat("Spacing (m)##rig", &spacing, 0.1f, 5.0f);
    } else if (layout == Layout::Arc) {
        ImGui::SliderFloat("Arc spread (deg)##rig",
                            &arcSpreadDeg, 10.0f, 360.0f);
        ImGui::SliderFloat("Arc radius (m)##rig",
                            &arcRadius, 0.5f, 30.0f);
    }
    ImGui::SliderFloat("Fixture phase##rig", &fixturePhase, 0.0f, 1.0f);
    ImGui::TextDisabled("0 = synced (sweep) | 1 = chase (figure-8 pursue)");

    ImGui::Separator();
    ImGui::TextDisabled("AIM (relative to %s)",
                        followCamera ? "camera forward" : "world +Y");
    ImGui::SliderFloat("Pan (deg)##spot",  &panDeg,  -180.0f, 180.0f);
    ImGui::SliderFloat("Tilt (deg)##spot", &tiltDeg, -90.0f,  90.0f);

    ImGui::Separator();
    ImGui::TextDisabled("LIGHT SHAPE");
    ImGui::Checkbox("Per-fixture colors##spot", &useFixtureColors);
    if (useFixtureColors) {
        // Ensure colors vector has at least fixtureCount entries.
        while (static_cast<int>(fixtureColors.size()) < fixtureCount) {
            // Default new entries to the shared color so transitions are smooth.
            fixtureColors.push_back(color);
        }
        ImGui::Indent(8.0f);
        for (int i = 0; i < fixtureCount; ++i) {
            ImGui::PushID(i);
            char lbl[24];
            std::snprintf(lbl, sizeof(lbl), "Fixture %d", i + 1);
            ImGui::ColorEdit3(lbl, &fixtureColors[i][0],
                               ImGuiColorEditFlags_PickerHueWheel
                               | ImGuiColorEditFlags_Float
                               | ImGuiColorEditFlags_NoInputs);
            ImGui::PopID();
        }
        ImGui::Unindent(8.0f);
    } else {
        ImGui::ColorEdit3("Color##spot",     &color[0],
                          ImGuiColorEditFlags_PickerHueWheel
                          | ImGuiColorEditFlags_Float);
    }
    ImGui::SliderFloat("Intensity##spot", &intensity, 0.0f, 30.0f);
    ImGui::SliderFloat("Range (m)##spot", &range,     0.5f, 200.0f);
    ImGui::SliderFloat("Inner cone (deg)##spot", &innerDeg, 0.5f, 60.0f);
    ImGui::SliderFloat("Outer cone (deg)##spot", &outerDeg, 0.5f, 60.0f);
    if (outerDeg < innerDeg) outerDeg = innerDeg;

    if (ImGui::CollapsingHeader("Motion pattern (3-axis)",
                                 ImGuiTreeNodeFlags_DefaultOpen)) {
        const char* patterns[] = {
            "Off", "Circle", "Figure-8", "Ballyhoo", "Wave",
            "Sweep", "Can-can", "Strobe + pan"
        };
        int pat = static_cast<int>(motionLFO.pattern);
        ImGui::SetNextItemWidth(170.0f);
        if (ImGui::Combo("Pattern##motion", &pat, patterns, 8)) {
            motionLFO.pattern = static_cast<MotionLFO::Pattern>(pat);
        }
        if (motionLFO.pattern != MotionLFO::Pattern::Off) {
            ImGui::Indent(8.0f);
            ImGui::SliderFloat("Freq (Hz)##m", &motionLFO.freqHz, 0.0f, 4.0f);
            ImGui::SliderFloat("Pan amp##m",   &motionLFO.panAmp,  0.0f, 180.0f);
            ImGui::SliderFloat("Tilt amp##m",  &motionLFO.tiltAmp, 0.0f, 90.0f);
            ImGui::SliderFloat("Int amp##m",   &motionLFO.intAmp,  0.0f, 20.0f);
            ImGui::SliderFloat("Phase##m",     &motionLFO.phase,   0.0f, 1.0f);
            ImGui::TextDisabled("Tip: copy this spot, change only Phase");
            ImGui::TextDisabled("for fan / wave effects across N fixtures.");
            ImGui::Unindent(8.0f);
        }
    }

    if (ImGui::CollapsingHeader("Per-axis LFO modulation")) {
        drawLFOWidget("pan",       panLFO,       180.0f);
        drawLFOWidget("tilt",      tiltLFO,      90.0f);
        drawLFOWidget("intensity", intensityLFO, 20.0f);
    }

    colorTag = color;
}

} // namespace spacegen
