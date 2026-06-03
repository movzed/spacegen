#include "HologramMaterialLayer.h"

#include "ModulatorBank.h"

#include "imgui.h"

#include <algorithm>

namespace spacegen {

namespace {

constexpr const char* kModulatorNames[] = {
    "None", "LFO 1", "LFO 2", "LFO 3", "LFO 4",
    "LFO 5", "LFO 6", "LFO 7", "LFO 8"
};

// Reusable axis-combo entries.
constexpr const char* kAxisNames[] = { "X", "Y", "Z" };

} // namespace

HologramMaterialLayer::HologramMaterialLayer() {
    name      = "Hologram";
    blendMode = BlendMode::Add;            // matches consumed-by-Structure pattern
    colorTag  = glm::vec3(0.40f, 0.85f, 1.00f);
}

uint32_t HologramMaterialLayer::effectMask() const {
    // Master enable derived from layer state + opacity: if the layer is
    // disabled or fully transparent the entire effect short-circuits in
    // the shader (cheaper than evaluating every sub-effect with zero
    // contribution).
    if (state != LayerState::Enabled) return 0;
    if (opacity <= 0.0f)              return 0;

    uint32_t m = HMASK_ENABLE;
    if (pureReplace)                       m |= HMASK_REPLACE;
    if (scanEnabled)                       m |= HMASK_SCAN;
    if (scanEnabled && scanHarmonic)       m |= HMASK_SCAN_HARMONIC;
    if (fresnelEnabled)                    m |= HMASK_FRESNEL;
    if (fresnelEnabled && flickerEnabled)  m |= HMASK_FLICKER;
    if (glitchEnabled)                     m |= HMASK_GLITCH;
    if (glitchEnabled && glitchRgbShift)   m |= HMASK_GLITCH_RGB_SHIFT;

    switch (wireMode) {
        case WireMode::Off:    break;
        case WireMode::Deriv:  m |= HMASK_WIRE_DERIV; break;
        case WireMode::Bary:   m |= HMASK_WIRE_BARY;  break;
    }

    if (hueEnabled)                        m |= HMASK_HUE_SHIFT;
    if (chromaAberration)                  m |= HMASK_CHROMA_ABER;
    if (dissolveEnabled)                   m |= HMASK_DISSOLVE;
    if (dissolveEnabled && dissolveNoise)  m |= HMASK_DISSOLVE_NOISE;
    return m;
}

float HologramMaterialLayer::effectiveOpacity(
    double t, const ModulatorBank* mods) const
{
    float eff = opacity;
    if (mods && opacityModSlot > 0) {
        eff += mods->eval(opacityModSlot, t) * opacityModDepth;
    }
    if (eff < 0.0f) eff = 0.0f;
    if (eff > 1.0f) eff = 1.0f;
    return eff;
}

// ---------------------------------------------------------------------------
// Inspector
// ---------------------------------------------------------------------------

void HologramMaterialLayer::drawInspector() {
    if (ImGui::CollapsingHeader("Master##holo",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SliderFloat("Opacity##holo", &opacity, 0.0f, 1.0f);
        ImGui::Checkbox("Pure-replace (suppress PBR)##holo", &pureReplace);
        ImGui::ColorEdit3("Master glow color##holo", &masterGlowColor[0],
                          ImGuiColorEditFlags_PickerHueWheel
                          | ImGuiColorEditFlags_Float);

        // Opacity modulator binding (same pattern as StructureLayer's
        // displaceModSlot / twistModSlot).
        ImGui::TextDisabled("Opacity LFO bind");
        ImGui::SetNextItemWidth(90.0f);
        ImGui::Combo("##holoOpMod", &opacityModSlot, kModulatorNames, 9);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        ImGui::SliderFloat("Depth##holoOpMod",
                            &opacityModDepth, 0.0f, 1.0f);

        // Live colorTag for the layer rack row.
        colorTag = masterGlowColor;
    }

    if (ImGui::CollapsingHeader("Scan lines##holo")) {
        ImGui::Checkbox("Enabled##scan",          &scanEnabled);
        ImGui::SameLine();
        ImGui::Checkbox("Harmonic (3×)##scan",    &scanHarmonic);
        ImGui::BeginDisabled(!scanEnabled);
        ImGui::SliderFloat("Frequency##scan",     &scanFreq,      20.0f, 800.0f);
        ImGui::SliderFloat("Speed (Hz)##scan",    &scanSpeed,     -4.0f,  4.0f);
        ImGui::SliderFloat("Intensity##scan",     &scanIntensity,  0.0f,  1.0f);
        ImGui::EndDisabled();
    }

    if (ImGui::CollapsingHeader("Fresnel rim##holo",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Enabled##fres",          &fresnelEnabled);
        ImGui::BeginDisabled(!fresnelEnabled);
        ImGui::ColorEdit3("Color##fres",          &fresnelColor[0],
                           ImGuiColorEditFlags_PickerHueWheel
                           | ImGuiColorEditFlags_Float);
        ImGui::SliderFloat("Power##fres",         &fresnelPower,     0.5f, 8.0f);
        ImGui::SliderFloat("Intensity##fres",     &fresnelIntensity, 0.0f, 4.0f);
        ImGui::EndDisabled();
    }

    if (ImGui::CollapsingHeader("Edge flicker##holo")) {
        ImGui::Checkbox("Enabled##flk",           &flickerEnabled);
        ImGui::BeginDisabled(!flickerEnabled || !fresnelEnabled);
        if (!fresnelEnabled) {
            ImGui::TextDisabled("(requires Fresnel rim)");
        }
        ImGui::ColorEdit3("Color##flk",           &flickerColor[0],
                           ImGuiColorEditFlags_PickerHueWheel
                           | ImGuiColorEditFlags_Float);
        ImGui::SliderFloat("Rate (Hz)##flk",      &flickerRate,     0.5f, 30.0f);
        ImGui::SliderFloat("Probability##flk",    &flickerProb,     0.0f,  1.0f);
        ImGui::SliderFloat("Duration##flk",       &flickerDuration, 0.05f, 1.0f);
        ImGui::EndDisabled();
    }

    if (ImGui::CollapsingHeader("Vertical glitch##holo")) {
        ImGui::Checkbox("Enabled##glt",           &glitchEnabled);
        ImGui::SameLine();
        ImGui::Checkbox("RGB shift##glt",         &glitchRgbShift);
        ImGui::BeginDisabled(!glitchEnabled);
        ImGui::SliderFloat("Rate (Hz)##glt",      &glitchRate,      0.5f, 30.0f);
        ImGui::SliderFloat("Bands##glt",          &glitchBands,     4.0f, 128.0f);
        ImGui::SliderFloat("Probability##glt",    &glitchProb,      0.0f,  1.0f);
        ImGui::SliderFloat("Amplitude##glt",      &glitchAmplitude, 0.0f,  0.20f);
        ImGui::EndDisabled();
    }

    if (ImGui::CollapsingHeader("Wireframe overlay##holo")) {
        const char* kWireModes[] = { "Off", "Deriv (any mesh)", "Baked bary" };
        int sel = static_cast<int>(wireMode);
        ImGui::SetNextItemWidth(180.0f);
        if (ImGui::Combo("Mode##wire", &sel, kWireModes, 3)) {
            wireMode = static_cast<WireMode>(sel);
        }
        ImGui::BeginDisabled(wireMode == WireMode::Off);
        ImGui::ColorEdit3("Color##wire",          &wireColor[0],
                           ImGuiColorEditFlags_PickerHueWheel
                           | ImGuiColorEditFlags_Float);
        ImGui::SliderFloat("Thickness (px)##wire", &wireThickness, 0.5f, 4.0f);
        ImGui::SliderFloat("Sharpness##wire",      &wireSharpness, 0.5f, 4.0f);
        ImGui::SliderFloat("Intensity##wire",      &wireIntensity, 0.0f, 4.0f);
        if (wireMode == WireMode::Bary) {
            ImGui::TextDisabled("Requires unindexed mesh (3× verts).");
            ImGui::TextDisabled("Call MetalRenderer::buildBarycentricBuffer()");
            ImGui::TextDisabled("on the active mesh first.");
        }
        ImGui::EndDisabled();
    }

    if (ImGui::CollapsingHeader("Color shift##holo")) {
        ImGui::Checkbox("Hue rotate##hue",         &hueEnabled);
        ImGui::BeginDisabled(!hueEnabled);
        ImGui::SliderFloat("Speed (rad/s)##hue",   &hueSpeed,  -1.0f, 1.0f);
        ImGui::SliderFloat("Offset (rad)##hue",    &hueOffset, -3.14159f, 3.14159f);
        ImGui::EndDisabled();

        ImGui::Separator();
        ImGui::Checkbox("Chromatic aberration##ca", &chromaAberration);
        ImGui::BeginDisabled(!chromaAberration);
        ImGui::SliderFloat("CA amount (UV)##ca",   &caAmount, 0.0f, 0.020f);
        ImGui::EndDisabled();
    }

    if (ImGui::CollapsingHeader("Dissolve plane##holo")) {
        ImGui::Checkbox("Enabled##dis",            &dissolveEnabled);
        ImGui::SameLine();
        ImGui::Checkbox("Noisy frontier##dis",     &dissolveNoise);
        ImGui::BeginDisabled(!dissolveEnabled);
        ImGui::SetNextItemWidth(120.0f);
        ImGui::Combo("Axis##dis",                  &dissolveAxis,
                     kAxisNames, 3);
        ImGui::SliderFloat("Origin (m)##dis",      &dissolveOrigin, -20.0f, 20.0f);
        ImGui::SliderFloat("Speed (m/s)##dis",     &dissolveSpeed,   -5.0f,  5.0f);
        ImGui::SliderFloat("Band (m)##dis",        &dissolveBand,    0.05f,  4.0f);
        ImGui::BeginDisabled(!dissolveNoise);
        ImGui::SliderFloat("Noise amp##dis",       &dissolveNoiseAmp, 0.0f, 2.0f);
        ImGui::EndDisabled();
        ImGui::EndDisabled();
    }

    ImGui::TextDisabled("Consumed by StructureLayer — no extra draw call.");
}

} // namespace spacegen
