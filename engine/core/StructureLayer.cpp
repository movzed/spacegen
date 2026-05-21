#include "StructureLayer.h"

#include "BeamLayer.h"
#include "DirectionalLightLayer.h"
#include "Scene.h"
#include "../backends/metal/MetalRenderer.h"

#include "imgui.h"

namespace spacegen {

StructureLayer::StructureLayer() {
    name      = "Structure";
    blendMode = BlendMode::Normal;
    colorTag  = glm::vec3(0.85f, 0.85f, 0.85f);
}

void StructureLayer::render(RenderContext& ctx) {
    if (!ctx.renderer) return;

    // Collect all enabled light layers from the bus. The structure shader
    // integrates them in one pass — we don't render visible beams in air.
    std::vector<const BeamLayer*>             spots;
    std::vector<const DirectionalLightLayer*> dirs;
    if (ctx.scene) {
        for (auto& l : ctx.scene->bus.layers) {
            if (!l) continue;
            if (l->state != LayerState::Enabled) continue;
            if (l->opacity <= 0.0f) continue;
            if (auto* b = dynamic_cast<const BeamLayer*>(l.get())) {
                spots.push_back(b);
            } else if (auto* d = dynamic_cast<const DirectionalLightLayer*>(l.get())) {
                dirs.push_back(d);
            }
        }
    }
    ctx.renderer->renderStructureMeshes(ctx, *this, spots, dirs);
}

void StructureLayer::drawInspector() {
    if (ImGui::CollapsingHeader("Output",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Lights only (transparent base)##out",
                         &emitLightsOnly);
        if (emitLightsOnly) {
            ImGui::TextDisabled("Structure body invisible; output alpha");
            ImGui::TextDisabled("tracks light contribution. Composite-ready.");
        }
    }
    if (ImGui::CollapsingHeader("Material",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::ColorEdit3("Base color##s",
                          &baseColor[0],
                          ImGuiColorEditFlags_PickerHueWheel
                          | ImGuiColorEditFlags_Float);
        ImGui::SliderFloat("Roughness##s", &roughness, 0.04f, 1.0f);
        ImGui::SliderFloat("Metallic##s",  &metallic,  0.0f,  1.0f);
        ImGui::SliderFloat("Ambient##s",   &ambient,   0.0f,  0.4f);
    }
    ImGui::TextDisabled("Add a Directional or Spot layer in the rack.");
}

} // namespace spacegen
