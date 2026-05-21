#include "BeamLayer.h"

#include "../backends/metal/MetalRenderer.h"

#include "imgui.h"

namespace spacegen {

BeamLayer::BeamLayer() {
    name      = "Spot";
    blendMode = BlendMode::Add;   // labelling only; structure pass accumulates
    colorTag  = glm::vec3(1.0f, 0.95f, 0.85f);
}

void BeamLayer::drawInspector() {
    ImGui::Checkbox("Follow camera##spot", &followCamera);
    if (followCamera) {
        ImGui::TextDisabled("origin & direction track the scene camera");
    } else {
        ImGui::DragFloat3 ("Origin##spot",    &origin[0],    0.05f, -20.0f, 20.0f);
        ImGui::SliderFloat3("Direction##spot",&direction[0], -1.0f, 1.0f);
    }
    ImGui::ColorEdit3 ("Color##spot",     &color[0],
                       ImGuiColorEditFlags_PickerHueWheel
                       | ImGuiColorEditFlags_Float);
    ImGui::SliderFloat("Intensity##spot", &intensity, 0.0f, 30.0f);
    ImGui::SliderFloat("Range (m)##spot", &range,     0.5f, 200.0f);
    ImGui::SliderFloat("Inner cone (deg)##spot", &innerDeg, 0.5f, 60.0f);
    ImGui::SliderFloat("Outer cone (deg)##spot", &outerDeg, 0.5f, 60.0f);
    // Keep outer >= inner so the falloff is well-defined.
    if (outerDeg < innerDeg) outerDeg = innerDeg;

    // Sync visual tag color in the layer rack with the spot color.
    colorTag = color;
}

} // namespace spacegen
