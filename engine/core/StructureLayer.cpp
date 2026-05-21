#include "StructureLayer.h"

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
    ctx.renderer->renderStructureMeshes(ctx, *this);
}

void StructureLayer::drawInspector() {
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
    if (ImGui::CollapsingHeader("Directional light",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Button("Reset direction##s")) {
            lightDirection = glm::vec3(0.4f, -0.6f, -0.6f);
        }
        ImGui::SliderFloat3("Direction##s",  &lightDirection[0], -1.0f, 1.0f);
        ImGui::ColorEdit3 ("Color##s",       &lightColor[0],
                           ImGuiColorEditFlags_PickerHueWheel
                           | ImGuiColorEditFlags_Float);
        ImGui::SliderFloat("Intensity##s",   &lightIntensity, 0.0f, 10.0f);
    }
}

} // namespace spacegen
