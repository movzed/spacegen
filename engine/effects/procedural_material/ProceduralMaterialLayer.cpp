#include "ProceduralMaterialLayer.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>

namespace spacegen {

namespace {

// Combo labels — must stay aligned with ProcPattern enum order.
constexpr const char* kPatternNames[] = {
    "Voronoi cells",
    "Perlin fbm",
    "Curl noise",
    "Hexagonal grid",
    "Checker (multi-scale)",
    "Concentric rings",
    "Voronoi shatter",
    "Wood grain",
    "Marble",
    "Brick wall",
};
static_assert(sizeof(kPatternNames) / sizeof(kPatternNames[0])
              == static_cast<int>(ProcPattern::Count),
              "kPatternNames must match ProcPattern::Count");

// Per-pattern "uses octaves" mask — used by the inspector to grey-out
// the octaves slider on patterns that don't run fbm.
constexpr bool kUsesOctaves[] = {
    false,   // Voronoi
    true,    // Perlin fbm
    true,    // Curl noise
    false,   // Hex
    true,    // Checker (multi-scale layering)
    false,   // Rings
    false,   // Voronoi shatter
    true,    // Wood
    true,    // Marble
    false,   // Brick
};

// Per-pattern contrast tooltip — explains what "contrast" does for each.
constexpr const char* kContrastDocs[] = {
    "< 0.5: filled cells. >= 0.5: crack lines.",
    "Posterise the fbm output around 0.5.",
    "Boost streamline visibility.",
    "Edge sharpness (anti-aliased).",
    "Hardness of black/white transitions.",
    "Ring band width (1 = thinnest).",
    "Gap width between cells.",
    "Grain band sharpness.",
    "Vein contrast.",
    "Mortar width vs brick body.",
};

// Apply per-colour opacity by lerping toward neutral grey (0.5).
// opacity == 0 → grey, opacity == 1 → original picker colour.
inline glm::vec3 desaturate(const glm::vec3& c, float opacity) {
    const float t = std::clamp(opacity, 0.0f, 1.0f);
    return glm::vec3(0.5f) + (c - glm::vec3(0.5f)) * t;
}

} // namespace

ProceduralMaterialLayer::ProceduralMaterialLayer() {
    name      = "Procedural material";
    blendMode = BlendMode::Normal;
    colorTag  = glm::vec3(0.42f, 0.78f, 0.55f);
}

void ProceduralMaterialLayer::render(RenderContext& /*ctx*/) {
    // Intentionally empty. ProceduralMaterialLayer is a *modifier* on the
    // structure pass — its parameters are collected by StructureLayer
    // during its own render() (via a dynamic_cast walk of the bus) and
    // packed into the structure shader's uniforms. See INTEGRATION.md
    // step 4 (StructureLayer collection loop).
    //
    // We keep an explicit render() so the layer registers in the bus
    // ordering / enable / opacity machinery uniformly with every other
    // layer.
}

void ProceduralMaterialLayer::drawInspector() {
    if (ImGui::CollapsingHeader("Pattern",
                                 ImGuiTreeNodeFlags_DefaultOpen)) {
        int p = static_cast<int>(pattern);
        if (ImGui::Combo("Type##pmtype", &p,
                          kPatternNames,
                          static_cast<int>(ProcPattern::Count))) {
            pattern = static_cast<ProcPattern>(
                std::clamp(p, 0, static_cast<int>(ProcPattern::Count) - 1));
        }
    }

    if (ImGui::CollapsingHeader("Palette",
                                 ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::ColorEdit3("Color A##pma",
                          &colorA[0],
                          ImGuiColorEditFlags_PickerHueWheel
                          | ImGuiColorEditFlags_Float);
        ImGui::SliderFloat("Opacity A##pma", &colorAOpacity, 0.0f, 1.0f);

        ImGui::ColorEdit3("Color B##pmb",
                          &colorB[0],
                          ImGuiColorEditFlags_PickerHueWheel
                          | ImGuiColorEditFlags_Float);
        ImGui::SliderFloat("Opacity B##pmb", &colorBOpacity, 0.0f, 1.0f);
        ImGui::TextDisabled("Opacity fades each colour toward neutral grey.");
    }

    if (ImGui::CollapsingHeader("Shape",
                                 ImGuiTreeNodeFlags_DefaultOpen)) {
        // Log-scaled scale slider: most useful range is 0.05..50 spanning
        // ~3 decades. We use SliderFloat with ImGuiSliderFlags_Logarithmic
        // for the canonical log-knob behaviour.
        ImGui::SliderFloat("Scale##pms", &scale, 0.05f, 50.0f,
                           "%.3f",
                           ImGuiSliderFlags_Logarithmic);

        ImGui::SliderFloat2("Anim drift XY##pmd", &animSpeed.x, -2.0f, 2.0f);
        ImGui::SliderFloat ("Anim time Z##pmtz",  &animSpeed.z,  0.0f, 4.0f);

        const int patIdx = static_cast<int>(pattern);
        ImGui::SliderFloat("Contrast##pmc", &contrast, 0.0f, 2.0f);
        if (patIdx >= 0 && patIdx < static_cast<int>(ProcPattern::Count)) {
            ImGui::TextDisabled("%s", kContrastDocs[patIdx]);
        }

        // Octaves: only meaningful for fbm-based patterns. Grey it out
        // otherwise — the value is still serialised but the slider is
        // visually disabled.
        const bool needOct = (patIdx >= 0
                              && patIdx < static_cast<int>(ProcPattern::Count))
                              ? kUsesOctaves[patIdx] : false;
        ImGui::BeginDisabled(!needOct);
        ImGui::SliderInt("Octaves##pmo", &octaves, 1, 8);
        ImGui::EndDisabled();
        if (!needOct) {
            ImGui::TextDisabled("(octaves only used by fbm patterns)");
        }
    }

    if (ImGui::CollapsingHeader("Blend",
                                 ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SliderFloat("Mix##pmm", &mix, 0.0f, 1.0f);
        ImGui::TextDisabled("0 = invisible, 1 = full strength.");

        // Layered-material compositing: how this layer blends onto the
        // layers below it, and an optional spatial mask.
        static const char* kBlendNames[] = {
            "Normal", "Add", "Multiply", "Screen", "Overlay"
        };
        int b = static_cast<int>(textureBlend);
        if (ImGui::Combo("Blend mode##pmbl", &b, kBlendNames,
                          static_cast<int>(ProcBlend::Count))) {
            textureBlend = static_cast<ProcBlend>(b);
        }

        static const char* kMaskNames[] = {
            "None", "Fresnel (edges)", "Height (Z)", "Noise", "Pattern luma"
        };
        int m = static_cast<int>(maskType);
        if (ImGui::Combo("Mask##pmmask", &m, kMaskNames,
                          static_cast<int>(ProcMask::Count))) {
            maskType = static_cast<ProcMask>(m);
        }
        if (maskType != ProcMask::None) {
            if (maskType == ProcMask::Noise) {
                ImGui::SliderFloat("Mask scale##pmms", &maskScale, 0.1f, 20.0f);
            }
            if (maskType == ProcMask::Fresnel) {
                ImGui::SliderFloat("Fresnel power##pmmp", &maskParam, 0.5f, 8.0f);
            } else if (maskType == ProcMask::HeightZ) {
                ImGui::SliderFloat("Height centre (m)##pmmp", &maskParam, -5.0f, 5.0f);
            }
            ImGui::Checkbox("Invert mask##pmmi", &maskInvert);
        }
        ImGui::TextDisabled("Stack several Procedural layers — they");
        ImGui::TextDisabled("composite bottom-to-top with these modes.");
    }
}

ProceduralMaterialUniforms packProceduralMaterial(
    const ProceduralMaterialLayer& l)
{
    ProceduralMaterialUniforms u{};

    // Resolve per-colour opacity at CPU side so the shader stays branch-
    // free on the palette path.
    const glm::vec3 a = desaturate(l.colorA, l.colorAOpacity);
    const glm::vec3 b = desaturate(l.colorB, l.colorBOpacity);
    u.colorA = glm::vec4(a, l.colorAOpacity);
    u.colorB = glm::vec4(b, l.colorBOpacity);

    // shape: x = scale, y = contrast, z = mix * opacity, w = pattern index
    // Multiplying mix by the layer's own opacity lets the layer fader on
    // the rack act as a global wet/dry without the operator having to
    // also re-pull the mix slider.
    const float effectiveMix =
        std::clamp(l.mix * std::clamp(l.opacity, 0.0f, 1.0f), 0.0f, 1.0f);
    u.shape = glm::vec4(std::max(l.scale, 1e-3f),
                        std::clamp(l.contrast, 0.0f, 2.0f),
                        effectiveMix,
                        static_cast<float>(static_cast<int>(l.pattern)));

    // anim: xy axis drift, z time multiplier, w octave count
    const int octClamped = std::clamp(l.octaves, 1, 8);
    u.anim = glm::vec4(l.animSpeed.x,
                       l.animSpeed.y,
                       std::max(l.animSpeed.z, 0.0f),
                       static_cast<float>(octClamped));

    // blend: x = blend mode, y = mask type, z = mask scale,
    //        w = mask param (negated to flag invert — the shader reads
    //        abs() for the value and sign for the invert toggle).
    float maskP = std::max(l.maskParam, 0.01f);
    if (l.maskInvert) maskP = -maskP;
    u.blend = glm::vec4(static_cast<float>(static_cast<int>(l.textureBlend)),
                        static_cast<float>(static_cast<int>(l.maskType)),
                        std::max(l.maskScale, 1e-3f),
                        maskP);
    return u;
}

ProceduralMaterialUniforms disabledProceduralMaterial() {
    ProceduralMaterialUniforms u{};
    u.colorA = glm::vec4(0.5f, 0.5f, 0.5f, 0.0f);
    u.colorB = glm::vec4(0.5f, 0.5f, 0.5f, 0.0f);
    // mix = 0 triggers the shader fast-path; pattern = 0; scale = 1;
    // contrast = 1; octaves = 1.
    u.shape  = glm::vec4(1.0f, 1.0f, 0.0f, 0.0f);
    u.anim   = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    return u;
}

} // namespace spacegen
