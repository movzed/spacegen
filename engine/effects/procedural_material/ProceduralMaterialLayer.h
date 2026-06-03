#pragma once
// ProceduralMaterialLayer — per-fragment procedural texture that REPLACES
// (or blends into) the StructureLayer's baseColor in the structure shader.
//
// No bitmap loads. The whole pattern library lives in MSL — this layer
// only pushes a small parameter block to the renderer through a global
// procedural-material state read by MetalRenderer::renderStructureMeshes.
//
// Insertion point in fs_main: BEFORE the existing Syphon mix, so the
// operator can stack a Syphon overlay on top of a procedurally shaded
// surface. See INTEGRATION.md for the shader patch.
//
// References:
//   - Worley 1996 (Voronoi / cellular)
//   - Bridson 2007 (curl noise)
//   - Inigo Quilez, iquilezles.org (fbm, hex, marble, palette canon)

#include "Layer.h"

namespace spacegen {

// Wire-compatible with the MSL switch in patterns.metal.inc. DO NOT
// reorder — the ImGui combo and the shader dispatcher index by these
// values.
enum class ProcPattern : int {
    Voronoi      = 0,
    PerlinFBM    = 1,
    CurlNoise    = 2,
    Hex          = 3,
    Checker      = 4,
    Rings        = 5,
    VoronoiShatter = 6,
    Wood         = 7,
    Marble       = 8,
    Brick        = 9,
    Count        = 10
};

// How this texture layer composites onto the albedo accumulated by the
// layers below it (node-based / layered material stack). The bottom layer
// always uses Normal against the structure base color.
enum class ProcBlend : int {
    Normal   = 0,   // straight over (lerp by opacity·mask)
    Add      = 1,
    Multiply = 2,
    Screen   = 3,
    Overlay  = 4,
    Count    = 5
};

// Optional spatial mask that gates where this layer contributes — the key
// to a node-based look (one texture only on edges, one only by height,
// one driven by noise, etc.). Evaluated in world space.
enum class ProcMask : int {
    None        = 0,   // full strength everywhere
    Fresnel     = 1,   // pow(1 - N·V, maskParam) — rim/edge emphasis
    HeightZ     = 2,   // smoothstep along world Z, centred at maskParam
    Noise       = 3,   // world-space fbm at maskScale
    PatternLuma = 4,   // luminance of this layer's own pattern
    Count       = 5
};

class ProceduralMaterialLayer : public ILayer {
public:
    ProceduralMaterialLayer();

    // This layer doesn't draw geometry; it's a *modifier* on the structure
    // pass. We classify it as an Effect so the rack groups it correctly,
    // but render() is a no-op — the actual work happens inside the
    // structure shader by reading our state from the bus.
    LayerKind   kind()      const override { return LayerKind::Effect; }
    const char* typeName()  const override { return "ProceduralMaterial"; }
    void        render(RenderContext& ctx) override;
    void        drawInspector() override;

    // ---- Pattern selector ----
    ProcPattern pattern = ProcPattern::PerlinFBM;

    // ---- Two-colour palette ----
    // colorA / colorB are sRGB picker values. colorA/Bopacity desaturates
    // each toward neutral grey (0.5) — useful when the operator wants the
    // pattern to *modulate* rather than *replace* the underlying baseColor.
    glm::vec3 colorA        = glm::vec3(0.05f, 0.05f, 0.07f);
    float     colorAOpacity = 1.0f;
    glm::vec3 colorB        = glm::vec3(0.88f, 0.82f, 0.70f);
    float     colorBOpacity = 1.0f;

    // ---- Shaping ----
    // scale: spatial frequency (1.0 = pattern unit ≈ uv unit).
    //        Displayed as log-slider, range 0.05..50.
    // animSpeed.xy: per-axis pattern drift (uv units per second).
    // animSpeed.z:  time multiplier for patterns that animate internally
    //                (Voronoi feature jitter, brick flicker, marble drift).
    // contrast:    0 = soft / smooth, 1 = neutral, 2 = hard / posterised.
    //               Pattern-specific meaning, see README.
    // octaves:     fbm octave count for noise-based patterns
    //               (Perlin, Curl, Wood, Marble). Clamped 1..8.
    float     scale       = 4.0f;
    glm::vec3 animSpeed   = glm::vec3(0.0f, 0.0f, 1.0f);
    float     contrast    = 1.0f;
    int       octaves     = 5;

    // ---- Blend ----
    // mix == 0 → invisible (shader fast-paths out, no pattern math runs)
    // mix == 1 → procedural fully replaces baseColor
    // The structure's baseColor (StructureLayer.baseColor × material
    // baseColorFactor × baseColorMap texture) is lerped against the
    // pattern output by this value before the Syphon mix runs.
    float     mix         = 1.0f;

    // ---- Layered-material compositing (node-based stack) ----
    // How this layer blends onto the albedo accumulated by layers below it,
    // and an optional spatial mask gating where it contributes. Multiple
    // ProceduralMaterialLayers in the bus stack in order (bottom = first
    // added). The bottom layer effectively blends over the structure base.
    ProcBlend textureBlend = ProcBlend::Normal;
    ProcMask  maskType      = ProcMask::None;
    float     maskScale     = 2.0f;    // Noise mask spatial frequency
    float     maskParam     = 3.0f;    // Fresnel power / Height centre (m)
    bool      maskInvert     = false;
};

// CPU-side mirror of the MSL uniform block. Packed to match the layout
// added at the end of the structure shader's Uniforms struct. See
// INTEGRATION.md.
struct ProceduralMaterialUniforms {
    glm::vec4 colorA;          // .rgb sRGB colour A, .a desaturate amount
    glm::vec4 colorB;          // .rgb sRGB colour B, .a desaturate amount
    glm::vec4 shape;           // .x scale, .y contrast, .z mix, .w pattern (int)
    glm::vec4 anim;            // .xy animSpeed.xy, .z animSpeed.z, .w octaves
    glm::vec4 blend;           // .x blendMode, .y maskType, .z maskScale, .w maskParam(+sign=invert)
};

// Pack a layer's state into the uniform block. Resolves the desaturation
// toward neutral grey (0.5) at CPU side so the shader stays branch-free
// on the palette path.
ProceduralMaterialUniforms packProceduralMaterial(const ProceduralMaterialLayer& l);

// "Disabled" block — pushed when no ProceduralMaterialLayer is in the
// bus, so the shader's mix == 0 fast path triggers and the structure
// renders normally.
ProceduralMaterialUniforms disabledProceduralMaterial();

} // namespace spacegen
