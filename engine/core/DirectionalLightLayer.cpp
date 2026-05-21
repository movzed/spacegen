#include "DirectionalLightLayer.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "imgui.h"

#include <cmath>

namespace spacegen {

namespace {

constexpr float kPi = 3.14159265358979323846f;

// Convert pan/tilt (deg) -> world-space direction (TOWARDS scene).
// Pan: yaw around world Z (Z-up), 0 = +Y. Tilt: pitch from horizontal.
glm::vec3 panTiltToDir(float panDeg, float tiltDeg) {
    float pr = panDeg  * kPi / 180.0f;
    float tr = tiltDeg * kPi / 180.0f;
    float cp = std::cos(pr), sp = std::sin(pr);
    float ct = std::cos(tr), st = std::sin(tr);
    // Base forward = +Y. Rotate pan around Z, then tilt up around X.
    // Final dir = (cp*ct*0 + (-sp)*ct*1 + 0, sp*0 + cp*1 - 0, st...) — let me redo:
    // Start: f0 = (0, 1, 0)
    // Pan around +Z: rot matrix on (x,y): x' = -sp, y' = cp
    // So after pan: (-sp, cp, 0)
    // Tilt around the local right axis (which is panned right = (cp, sp, 0)):
    //   The vertical component (Z) = st
    //   The forward component (XY) is scaled by ct
    return glm::vec3(-sp * ct, cp * ct, st);
}

void drawLFOWidget(const char* label, LFO& lfo) {
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
        ImGui::SliderFloat("amp",  &lfo.amplitude, 0.0f, 180.0f);
        ImGui::SliderFloat("freq", &lfo.freqHz,    0.0f, 8.0f);
        ImGui::SliderFloat("ph",   &lfo.phase,     0.0f, 1.0f);
        ImGui::Unindent(16.0f);
    }
    ImGui::PopID();
}

} // anon

DirectionalLightLayer::DirectionalLightLayer() {
    name      = "Directional";
    blendMode = BlendMode::Add;
    colorTag  = glm::vec3(0.95f, 0.92f, 0.80f);
}

glm::vec3 DirectionalLightLayer::directionAtTime(double t) const {
    float pan  = panDeg  + panLFO.eval(t);
    float tilt = tiltDeg + tiltLFO.eval(t);
    return glm::normalize(panTiltToDir(pan, tilt));
}

float DirectionalLightLayer::intensityAtTime(double t) const {
    return std::max(0.0f, intensity + intensityLFO.eval(t));
}

void DirectionalLightLayer::drawInspector() {
    ImGui::SliderFloat("Pan (deg)##d",  &panDeg,  -180.0f, 180.0f);
    ImGui::SliderFloat("Tilt (deg)##d", &tiltDeg, -90.0f,  90.0f);
    ImGui::ColorEdit3 ("Color##d",      &color[0],
                       ImGuiColorEditFlags_PickerHueWheel
                       | ImGuiColorEditFlags_Float);
    ImGui::SliderFloat("Intensity##d",  &intensity, 0.0f, 8.0f);
    if (ImGui::CollapsingHeader("LFO modulation")) {
        drawLFOWidget("pan",       panLFO);
        drawLFOWidget("tilt",      tiltLFO);
        drawLFOWidget("intensity", intensityLFO);
    }
    colorTag = color;
}

} // namespace spacegen
