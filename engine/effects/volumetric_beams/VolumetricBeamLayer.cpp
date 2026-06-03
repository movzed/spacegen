#include "VolumetricBeamLayer.h"

#include "BeamLayer.h"
#include "ModulatorBank.h"
#include "Scene.h"
#include "../backends/metal/MetalRenderer.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace spacegen {

namespace {

constexpr int   kMaxSpots         = 32;
constexpr float kPi               = 3.14159265358979323846f;
constexpr float kAnisotropyClamp  = 0.99f;

// Mirror of the MSL `VolumetricSpot` struct in volumetric.metal.inc. The
// renderer reads `VolumetricBeamPackedSpot` arrays and the master uniforms
// directly from this header (so keep these layouts byte-identical to the
// shader-side counterparts).
struct PackedSpot {
    glm::vec4 posIntensity;    // .xyz worldPos, .w intensity (already * opacity * perFixtureMul)
    glm::vec4 dirRange;        // .xyz dir (FROM light), .w effective range
    glm::vec4 colorInner;      // .rgb color, .a innerCos
    glm::vec4 outerMul;        // .x outerCos
};

// Cone parameters cached per BeamLayer, so we don't recompute the cosines
// for every fixture in the inner loop.
struct BeamCache {
    float innerCos;
    float outerCos;
    glm::vec3 baseFwd;
    const ModulatorBank* mods;
    int  firstSlot;   // index in flat fixture list where this rig's fixtures begin
    int  fixtureCount;
};

} // anon

VolumetricBeamLayer::VolumetricBeamLayer() {
    name      = "Volumetric Beams";
    blendMode = BlendMode::Add;
    colorTag  = glm::vec3(0.55f, 0.45f, 0.95f);
}

void VolumetricBeamLayer::render(RenderContext& ctx) {
    if (!ctx.renderer || !ctx.scene) return;
    if (state != LayerState::Enabled || opacity <= 0.0f) return;

    // Early-out — at zero density the layer is invisible. Saves the post-
    // process fragment pass entirely.
    if (density <= 1e-6f) return;

    // ---- Walk the bus exactly the way StructureLayer does -------------------
    // We need both the spotlight params and the per-rig fixture decomposition
    // so the inspector can label per-fixture multipliers in bus order.
    const double t = ctx.elapsedSeconds;
    const ModulatorBank* mods = &ctx.scene->modulators;

    PackedSpot packed[kMaxSpots]{};
    int spotCount = 0;
    activeFixtureCount = 0;

    for (auto& l : ctx.scene->bus.layers) {
        if (!l) continue;
        if (l->state != LayerState::Enabled) continue;
        if (l->opacity <= 0.0f) continue;
        const BeamLayer* beam = dynamic_cast<const BeamLayer*>(l.get());
        if (!beam) continue;

        glm::vec3 baseFwd = beam->followCamera
            ? glm::normalize(ctx.cameraForward)
            : glm::vec3(0.0f, 1.0f, 0.0f);

        float innerRad = beam->innerDeg * kPi / 180.0f;
        float outerRad = beam->outerDeg * kPi / 180.0f;
        if (outerRad < innerRad) outerRad = innerRad;
        const float innerCos = std::cos(innerRad);
        const float outerCos = std::cos(outerRad);

        auto positions = beam->fixturePositions(ctx);
        const int N = static_cast<int>(positions.size());

        for (int i = 0; i < N && spotCount < kMaxSpots; ++i, ++spotCount) {
            const int slot = spotCount;
            const glm::vec3 origin = positions[i];
            const glm::vec3 dir =
                beam->directionAtTimeForFixture(t, i, N, baseFwd, mods);
            const float beamI =
                beam->intensityAtTimeForFixture(t, i, N, mods) * beam->opacity;
            const glm::vec3 col = beam->colorForFixture(i);

            // Effective intensity: per-fixture multiplier × layer opacity.
            // Clamped to 0 so a negative multiplier just disables.
            const float perFx =
                std::max(0.0f, perFixtureMul[slot]) * this->opacity;
            const float effI = beamI * perFx;

            // Effective range: either the beam's own range or the operator's
            // cap on cone length, whichever is smaller.
            float effRange = beam->range;
            if (maxConeLength >= 0.0f) {
                effRange = std::min(effRange, maxConeLength);
            }

            packed[slot].posIntensity = glm::vec4(origin, effI);
            packed[slot].dirRange     = glm::vec4(dir, effRange);
            packed[slot].colorInner   = glm::vec4(col, innerCos);
            packed[slot].outerMul     = glm::vec4(outerCos, 0.0f, 0.0f, 0.0f);

            // Inspector book-keeping (label cache).
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%s · %d",
                           beam->name.empty() ? "Spot" : beam->name.c_str(),
                           i + 1);
            fixtureLabels[slot] = buf;
            ++activeFixtureCount;
        }
        if (spotCount >= kMaxSpots) break;
    }
    // Clear out any stale labels past the live range.
    for (int s = activeFixtureCount; s < kMaxFixtures; ++s) {
        fixtureLabels[s].clear();
    }

    // Nothing to render? Skip the pass instead of issuing a zero-light
    // fullscreen-quad that would just clear nothing.
    if (spotCount == 0) return;

    // ---- Dispatch the post-process -----------------------------------------
    // We hand the packed array to the renderer's new entry point; see
    // INTEGRATION.md for the shape and rationale. The renderer owns the
    // depth texture and the (volumetric-specific) pipeline state.
    MetalRenderer::VolumetricUniforms vu{};
    vu.spots         = packed;
    vu.spotCount     = spotCount;
    vu.density       = density;
    vu.anisotropy    = std::clamp(anisotropy, -kAnisotropyClamp, kAnisotropyClamp);
    vu.sampleCount   = std::clamp(sampleCount, 4, 128);
    vu.jitterStrength = std::clamp(jitterStrength, 0.0f, 1.0f);
    vu.tint          = tint;
    vu.beerLambert   = beerLambertExtinction;
    vu.layerOpacity  = opacity;

    ctx.renderer->renderVolumetricBeams(ctx, vu);
}

void VolumetricBeamLayer::drawInspector() {
    ImGui::TextDisabled("Concert haze — \"cones of light in air\".");
    ImGui::TextDisabled("Reads scene depth; raymarches every spotlight.");

    if (ImGui::CollapsingHeader("Master",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SliderFloat("Density##vb",       &density,        0.0f, 0.10f, "%.4f");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Scattering coefficient σ_s.\n"
                              "0 disables (no perf cost).\n"
                              "~0.005-0.03 is concert-hazer range.\n"
                              "~0.05+ enters mystical-fog territory.");
        }
        ImGui::SliderFloat("Anisotropy g##vb",  &anisotropy,    -0.95f, 0.95f);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Henyey-Greenstein g parameter.\n"
                              "0 = isotropic scatter.\n"
                              "+0.7 = forward scatter (theatre haze, fog).\n"
                              "Negative = back-scatter (rare in physical media).");
        }
        ImGui::ColorEdit3("Tint##vb", &tint[0],
                          ImGuiColorEditFlags_PickerHueWheel
                          | ImGuiColorEditFlags_Float);

        ImGui::Checkbox("Beer-Lambert extinction##vb",
                         &beerLambertExtinction);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Apply absorption along the view ray (deeper\n"
                              "samples see less of the camera). Slightly more\n"
                              "physical, marginally more expensive. Off by\n"
                              "default for the cleaner stage look.");
        }

        bool capOn = (maxConeLength >= 0.0f);
        if (ImGui::Checkbox("Cap cone length##vb", &capOn)) {
            maxConeLength = capOn ? 20.0f : -1.0f;
        }
        if (capOn) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(120.0f);
            ImGui::SliderFloat("##capm", &maxConeLength, 0.5f, 200.0f, "%.1f m");
        }
    }

    if (ImGui::CollapsingHeader("Raymarch",
                                 ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SliderInt("Samples##vb",         &sampleCount,    4,   128);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Number of raymarch steps per fragment.\n"
                              "Linear perf cost. 16=fast, 48=default,\n"
                              "128=reference quality. See README for budget.");
        }
        ImGui::SliderFloat("Jitter##vb",        &jitterStrength, 0.0f, 1.0f);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Per-pixel dither strength for banding mitigation.\n"
                              "0 = banding visible. 0.85 = banding broken,\n"
                              "very faint grain. 1 = full ±0.5-step dither.");
        }
    }

    if (ImGui::CollapsingHeader("Per-fixture contribution")) {
        if (activeFixtureCount == 0) {
            ImGui::TextDisabled("No spotlights in the bus yet — add a");
            ImGui::TextDisabled("BeamLayer to drive the cones.");
        } else {
            ImGui::TextDisabled("Active fixtures: %d / %d",
                                 activeFixtureCount, kMaxFixtures);
            for (int i = 0; i < activeFixtureCount; ++i) {
                ImGui::PushID(i);
                ImGui::SetNextItemWidth(180.0f);
                ImGui::SliderFloat(fixtureLabels[i].c_str(),
                                    &perFixtureMul[i], 0.0f, 4.0f);
                ImGui::PopID();
            }
            if (activeFixtureCount > 1) {
                if (ImGui::Button("All = 1.0")) {
                    for (int i = 0; i < activeFixtureCount; ++i) {
                        perFixtureMul[i] = 1.0f;
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("All = 0.0")) {
                    for (int i = 0; i < activeFixtureCount; ++i) {
                        perFixtureMul[i] = 0.0f;
                    }
                }
            }
        }
    }

    colorTag = tint * (density > 0.0f ? 1.0f : 0.25f);
}

} // namespace spacegen
