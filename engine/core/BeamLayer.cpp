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

void BeamLayer::drawInspector() {
    ImGui::Checkbox("Follow camera##spot", &followCamera);
    if (followCamera) {
        ImGui::TextDisabled("origin tracks the scene camera");
    } else {
        ImGui::DragFloat3("Origin##spot", &origin[0], 0.05f, -20.0f, 20.0f);
    }

    ImGui::Separator();
    ImGui::TextDisabled("AIM (relative to %s)",
                        followCamera ? "camera forward" : "world +Y");
    ImGui::SliderFloat("Pan (deg)##spot",  &panDeg,  -180.0f, 180.0f);
    ImGui::SliderFloat("Tilt (deg)##spot", &tiltDeg, -90.0f,  90.0f);

    ImGui::Separator();
    ImGui::TextDisabled("LIGHT SHAPE");
    ImGui::ColorEdit3 ("Color##spot",     &color[0],
                       ImGuiColorEditFlags_PickerHueWheel
                       | ImGuiColorEditFlags_Float);
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
