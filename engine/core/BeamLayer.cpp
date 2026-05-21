#include "BeamLayer.h"

#include "../backends/metal/MetalRenderer.h"

#include "imgui.h"

namespace spacegen {

BeamLayer::BeamLayer() {
    name      = "Beam";
    blendMode = BlendMode::Add;
    // Slight blue-tint colorTag by default; will get tinted by `color` later.
    colorTag  = glm::vec3(0.30f, 0.55f, 1.00f);
}

void BeamLayer::render(RenderContext& ctx) {
    if (!ctx.renderer) return;
    ctx.renderer->renderBeam(ctx, *this);
}

void BeamLayer::drawInspector() {
    ImGui::DragFloat3("Origin##b",     &origin[0],    0.05f, -10.0f, 10.0f);
    ImGui::SliderFloat3("Direction##b",&direction[0], -1.0f, 1.0f);
    ImGui::ColorEdit3 ("Color##b",     &color[0],
                       ImGuiColorEditFlags_PickerHueWheel
                       | ImGuiColorEditFlags_Float);
    ImGui::SliderFloat("Intensity##b", &intensity, 0.0f, 5.0f);
    ImGui::SliderFloat("Range (m)##b", &range,     0.5f, 40.0f);
    ImGui::SliderFloat("Cone (deg)##b",&coneDeg,   1.0f, 60.0f);
    ImGui::SliderFloat("Falloff##b",   &falloff,   0.5f, 6.0f);
    ImGui::SliderInt  ("Steps##b",     &steps,     8,    128);

    // Sync visual tag color in the layer rack with the beam color.
    colorTag = color;
}

} // namespace spacegen
