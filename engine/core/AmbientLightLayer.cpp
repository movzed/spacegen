#include "AmbientLightLayer.h"

#include "imgui.h"

namespace spacegen {

AmbientLightLayer::AmbientLightLayer() {
    name      = "Ambient";
    blendMode = BlendMode::Add;
    colorTag  = glm::vec3(0.55f, 0.55f, 0.60f);
}

void AmbientLightLayer::drawInspector() {
    ImGui::SliderFloat("Intensity##amb", &intensity, 0.0f, 1.0f);
    ImGui::ColorEdit3 ("Tint##amb",      &color[0],
                       ImGuiColorEditFlags_PickerHueWheel
                       | ImGuiColorEditFlags_Float);
    ImGui::TextDisabled("Multiplies the structure base color.");
    ImGui::TextDisabled("Stack several for tinted fills (warm + cool).");
    ImGui::TextDisabled("Disable for a dark stage where only spots light up.");
    colorTag = color;
}

} // namespace spacegen
