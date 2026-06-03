#include "MeshFractureLayer.h"

#include "../../core/ModulatorBank.h"

#include <algorithm>

#include "imgui.h"

namespace spacegen {

MeshFractureLayer::MeshFractureLayer() {
    name      = "Mesh Fracture";
    blendMode = BlendMode::Normal;
    colorTag  = glm::vec3(0.92f, 0.42f, 0.18f);  // burnt orange
}

float MeshFractureLayer::effectiveAmount(double t,
                                        const ModulatorBank* mods) const {
    float a = amount;
    if (mods) {
        a += mods->eval(amountModSlot, t) * amountModDepth;
    }
    return std::clamp(a, 0.0f, 1.0f);
}

bool MeshFractureLayer::active(double t, const ModulatorBank* mods) const {
    if (state != LayerState::Enabled) return false;
    if (mode == 0)                    return false;
    return effectiveAmount(t, mods) > 1e-4f;
}

void MeshFractureLayer::drawInspector() {
    static const char* kModNames[] = {
        "None", "LFO 1", "LFO 2", "LFO 3", "LFO 4",
        "LFO 5", "LFO 6", "LFO 7", "LFO 8"
    };

    if (ImGui::CollapsingHeader("Mode", ImGuiTreeNodeFlags_DefaultOpen)) {
        bool c = (mode & ModeCracks)   != 0;
        bool d = (mode & ModeDissolve) != 0;
        bool e = (mode & ModeExplode)  != 0;
        bool g = (mode & ModeGlitch)   != 0;
        if (ImGui::Checkbox("Animated cracks##frc",   &c)) { mode = c ? (mode | ModeCracks)   : (mode & ~ModeCracks); }
        if (ImGui::Checkbox("Dissolve burn##frd",     &d)) { mode = d ? (mode | ModeDissolve) : (mode & ~ModeDissolve); }
        if (ImGui::Checkbox("Per-shard explode##fre", &e)) { mode = e ? (mode | ModeExplode)  : (mode & ~ModeExplode); }
        if (ImGui::Checkbox("Glitch tear##frg",       &g)) { mode = g ? (mode | ModeGlitch)   : (mode & ~ModeGlitch); }
    }

    if (ImGui::CollapsingHeader("Amount (master)",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SliderFloat("Amount##frac", &amount, 0.0f, 1.0f);
        ImGui::SetNextItemWidth(90.0f);
        ImGui::Combo("##fracmod", &amountModSlot, kModNames, 9);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        ImGui::SliderFloat("Depth##fracmd", &amountModDepth, 0.0f, 2.0f);
    }

    if (mode & ModeCracks) {
        if (ImGui::CollapsingHeader("Cracks",
                                    ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::SliderFloat("Density (c/m)##fcd", &crackDensity, 0.2f, 8.0f);
            ImGui::SliderFloat("Edge width##fcw",     &crackWidth,   0.005f, 0.30f);
            ImGui::SliderFloat("Cell jitter##fcj",    &crackJitter,  0.0f, 1.0f);
            ImGui::SliderFloat("Jitter speed##fcjs",  &crackJitterSpeed, 0.0f, 2.0f);
            ImGui::SliderFloat("Darken##fcd2",        &crackDarken,  0.0f, 1.0f);
            ImGui::ColorEdit3("Glow color##fcc",
                              &crackGlowColor[0],
                              ImGuiColorEditFlags_PickerHueWheel
                              | ImGuiColorEditFlags_Float);
            ImGui::SliderFloat("Glow##fcg", &crackGlow, 0.0f, 8.0f);
        }
    }

    if (mode & ModeDissolve) {
        if (ImGui::CollapsingHeader("Dissolve burn",
                                    ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Radial##fdr", &useRadialDissolve);
            ImGui::InputFloat3("Burn point##fdp", &burnPoint[0]);
            ImGui::SliderFloat("Noise scale##fdn", &dissolveScale, 0.1f, 12.0f);
            ImGui::SliderFloat("Speed##fdsp",      &dissolveSpeed, -1.0f, 1.0f);
            ImGui::SliderFloat("Radius (m)##fdrad", &dissolveRadius, 0.5f, 50.0f);
            ImGui::SliderFloat("Radial bias##fdrb", &dissolveRadialBias, 0.0f, 1.5f);
            ImGui::SliderFloat("Edge width##fdew",  &dissolveEdgeWidth, 0.005f, 0.30f);
            ImGui::SliderFloat("Edge glow##fdeg",   &dissolveEdgeGlow,  0.0f, 8.0f);
            ImGui::ColorEdit3("Edge color##fdec",
                              &dissolveEdgeColor[0],
                              ImGuiColorEditFlags_PickerHueWheel
                              | ImGuiColorEditFlags_Float);
        }
    }

    if (mode & ModeExplode) {
        if (ImGui::CollapsingHeader("Per-shard explode",
                                    ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::SliderFloat("Shard density (c/m)##fsd", &shardDensity, 0.3f, 5.0f);
            ImGui::SliderFloat("Direction jitter##fsj",     &shardJitter,  0.0f, 1.5f);
            ImGui::SliderFloat("Spin##fss",                  &shardSpin,    0.0f, 1.0f);
            ImGui::SliderFloat("Strength (m)##fst",          &explodeStrength, 0.0f, 6.0f);
        }
    }

    if (mode & ModeGlitch) {
        if (ImGui::CollapsingHeader("Glitch tear",
                                    ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::SliderFloat("Frequency (bands/m)##fgf", &glitchFrequency, 0.5f, 30.0f);
            ImGui::SliderFloat("Magnitude (m)##fgm",        &glitchMagnitude, 0.0f, 2.0f);
            ImGui::SliderFloat("Speed (Hz)##fgs",           &glitchSpeed,     0.1f, 30.0f);
            ImGui::SliderFloat("Dropout##fgd",              &glitchDropout,   0.0f, 0.8f);
        }
    }

    if (mode == 0) {
        ImGui::TextDisabled("Pick a mode above to enable the fracture pass.");
    }
}

} // namespace spacegen
