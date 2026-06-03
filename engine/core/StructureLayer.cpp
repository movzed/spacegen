#include "StructureLayer.h"

#include "AmbientLightLayer.h"
#include "BeamLayer.h"
#include "DirectionalLightLayer.h"
#include "Scene.h"
#include "SyphonInputLayer.h"
#include "../effects/light_cloner/LightClonerLayer.h"
#include "../effects/area_light/AreaLightLayer.h"
#include "../effects/hologram/HologramMaterialLayer.h"
#include "../effects/procedural_material/ProceduralMaterialLayer.h"
#include "../effects/mesh_deformation/MeshDeformationLayer.h"
#include "../effects/mesh_fracture/MeshFractureLayer.h"
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
    std::vector<const AreaLightLayer*>        areas;
    glm::vec3 ambientColor(0.0f);
    MTL::Texture*  syphonTex   = nullptr;
    float          syphonMix   = 0.0f;
    glm::vec3      syphonTint(1.0f);
    bool           syphonFlipY = false;
    float          syphonProjFlatMix = 0.0f;
    float          syphonProjFlatThresh = 0.05f;
    std::vector<VirtualSpot> virtualSpots;
    // Surface-FX MVP packing: each effect contributes its master opacity
    // to one slot of surfaceFx. The shader knows what to do with each.
    float surfaceHoloOpacity      = 0.0f;
    float surfaceProcMix          = 0.0f;
    float surfaceProcScale        = 2.0f;
    float surfaceDeformAmount     = 0.0f;
    float surfaceDeformScale      = 1.5f;
    float surfaceFractureAmount   = 0.0f;
    float surfaceFractureSeed     = 0.0f;
    if (ctx.scene) {
        for (auto& l : ctx.scene->bus.layers) {
            if (!l) continue;
            if (l->state != LayerState::Enabled) continue;
            if (l->opacity <= 0.0f) continue;
            if (auto* b = dynamic_cast<const BeamLayer*>(l.get())) {
                spots.push_back(b);
            } else if (auto* d = dynamic_cast<const DirectionalLightLayer*>(l.get())) {
                dirs.push_back(d);
            } else if (auto* a = dynamic_cast<const AmbientLightLayer*>(l.get())) {
                ambientColor += a->color * (a->intensity * a->opacity);
            } else if (auto* c = dynamic_cast<const LightClonerLayer*>(l.get())) {
                c->expandSpots(ctx, virtualSpots);
            } else if (auto* ar = dynamic_cast<AreaLightLayer*>(l.get())) {
                // Idempotent: bus has likely already called ar->render()
                // (which calls update()) earlier this frame, but tick again
                // so a freshly-added layer is ready on its first frame.
                ar->update(ctx);
                areas.push_back(ar);
            } else if (auto* h = dynamic_cast<const HologramMaterialLayer*>(l.get())) {
                surfaceHoloOpacity = std::max(surfaceHoloOpacity, h->opacity);
            } else if (auto* p = dynamic_cast<const ProceduralMaterialLayer*>(l.get())) {
                surfaceProcMix   = std::max(surfaceProcMix, p->opacity);
                surfaceProcScale = 2.0f + 4.0f * p->opacity;
            } else if (auto* d = dynamic_cast<const MeshDeformationLayer*>(l.get())) {
                surfaceDeformAmount = std::max(surfaceDeformAmount,
                                                d->opacity * 0.20f);
                surfaceDeformScale  = 1.5f;
            } else if (auto* f = dynamic_cast<const MeshFractureLayer*>(l.get())) {
                // Default opacity is 1.0 on every ILayer. If we passed
                // that straight through, the shader's discard threshold
                // (> 0.6) annihilated the structure as soon as the layer
                // was added. Scale by 0.5 so opacity=1.0 produces visible
                // cracks WITHOUT the discard tier — operator dials past
                // ~1.2 (i.e. opacity 1 + masterAmount > 1) to start
                // shattering whole cells, which they have to do
                // intentionally.
                surfaceFractureAmount = std::max(surfaceFractureAmount,
                                                  f->opacity * 0.5f);
                surfaceFractureSeed   = static_cast<float>(f->id) * 0.137f;
            } else if (auto* s = dynamic_cast<SyphonInputLayer*>(l.get())) {
                if (!syphonTex) {
                    syphonTex   = s->currentTexture();
                    syphonMix   = s->mix * s->opacity;
                    syphonTint  = s->tint;
                    syphonFlipY = s->flipY;
                    syphonProjFlatMix    = s->projectorOnFlatMix;
                    syphonProjFlatThresh = s->projectorFlatnessThreshold;
                }
            }
        }
    }
    glm::vec4 surfaceFxVec(surfaceHoloOpacity, surfaceProcMix,
                            surfaceDeformAmount, surfaceFractureAmount);
    glm::vec4 surfaceFxParamsVec(surfaceProcScale, surfaceDeformScale,
                                   surfaceFractureSeed,
                                   static_cast<float>(ctx.elapsedSeconds));
    ctx.renderer->renderStructureMeshes(ctx, *this, spots, dirs,
                                          ambientColor,
                                          syphonTex, syphonMix, syphonTint,
                                          syphonFlipY,
                                          showStretchHeatmap,
                                          stretchMetric, stretchUV,
                                          syphonProjFlatMix,
                                          syphonProjFlatThresh,
                                          virtualSpots,
                                          surfaceFxVec,
                                          surfaceFxParamsVec,
                                          areas);
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
    }
    if (ImGui::CollapsingHeader("3D mesh effects",
                                 ImGuiTreeNodeFlags_DefaultOpen)) {
        static const char* kModNames[] = {
            "None", "LFO 1", "LFO 2", "LFO 3", "LFO 4",
            "LFO 5", "LFO 6", "LFO 7", "LFO 8"
        };
        ImGui::TextDisabled("Displace (along normals + noise)");
        ImGui::SliderFloat("Amount (m)##d", &displaceAmount, 0.0f, 2.0f);
        ImGui::SliderFloat("Noise scale##d", &displaceScale, 0.1f, 10.0f);
        ImGui::SetNextItemWidth(90.0f);
        ImGui::Combo("##dmod", &displaceModSlot, kModNames, 9);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        ImGui::SliderFloat("D depth##dm", &displaceModDepth, 0.0f, 2.0f);

        ImGui::Separator();
        ImGui::TextDisabled("Twist (around Z by height)");
        ImGui::SliderFloat("Twist amount##t", &twistAmount, -2.0f, 2.0f);
        ImGui::SetNextItemWidth(90.0f);
        ImGui::Combo("##tmod", &twistModSlot, kModNames, 9);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        ImGui::SliderFloat("T depth##tm", &twistModDepth, 0.0f, 2.0f);
    }
    ImGui::TextDisabled("Add Ambient / Directional / Spot layers");
    ImGui::TextDisabled("from the rack to light the structure.");
}

} // namespace spacegen
