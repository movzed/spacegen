#pragma once
// HologramMaterialLayer — replaces (or overlays) the structure mesh's PBR
// material with a sci-fi hologram look. Composable from seven sub-effects:
// scan lines, Fresnel rim, edge flicker, vertical glitch bars, wireframe
// overlay, color shift, and Z-fade dissolve.
//
// Consumed by StructureLayer the same way Beam / Directional / Ambient
// layers are: this layer's render() is a no-op; StructureLayer discovers
// it on the bus via dynamic_cast and packs its parameters into the
// structure uniforms. The existing structure shader (kStructurePbrMSL)
// applies the hologram math after PBR lighting has been integrated.
//
// See README.md for the per-sub-effect algorithm spec, MSL implementation,
// and the EFFECT_MASK bit layout used to toggle sub-effects cheaply.

#include "Layer.h"

namespace spacegen {

class HologramMaterialLayer : public ILayer {
public:
    HologramMaterialLayer();

    LayerKind   kind()     const override { return LayerKind::Generator; }
    const char* typeName() const override { return "Hologram Material"; }
    void        render(RenderContext& /*ctx*/) override {}  // consumed by StructureLayer
    void        drawInspector() override;

    // ---- Effect-mask bit layout. Must match shader (hologram.metal.inc) ----
    // The MSL compiler constant-folds branches gated on these bits, so
    // disabled sub-effects cost ~0 GPU instructions.
    enum EffectMask : uint32_t {
        HMASK_ENABLE            = 1u << 0,   // master gate; layer no-op when 0
        HMASK_REPLACE           = 1u << 1,   // suppress PBR base color & lighting
        HMASK_SCAN              = 1u << 2,
        HMASK_SCAN_HARMONIC     = 1u << 3,
        HMASK_FRESNEL           = 1u << 4,
        HMASK_FLICKER           = 1u << 5,
        HMASK_GLITCH            = 1u << 6,
        HMASK_GLITCH_RGB_SHIFT  = 1u << 7,
        HMASK_WIRE_DERIV        = 1u << 8,   // wireframe via fwidth(worldPos)
        HMASK_WIRE_BARY         = 1u << 9,   // wireframe via baked barycentrics
        HMASK_HUE_SHIFT         = 1u << 10,
        HMASK_CHROMA_ABER       = 1u << 11,
        HMASK_DISSOLVE          = 1u << 12,
        HMASK_DISSOLVE_NOISE    = 1u << 13,
    };

    // ---- Master ----
    // `opacity` (inherited from ILayer) cross-fades between the underlying
    // PBR result and the hologram result. 0 = pure PBR, 1 = pure hologram.
    // `pureReplace` is the HMASK_REPLACE shortcut — when true, the PBR
    // pipeline output is replaced entirely; when false, the hologram math
    // is added on top of the PBR result and `opacity` controls the blend.
    bool        pureReplace        = false;

    // Master glow color — multiplies the rim and the wireframe (the two
    // emissive contributions). Lets the operator change "the color of the
    // hologram" from one slider without touching each sub-effect.
    glm::vec3   masterGlowColor    = glm::vec3(0.40f, 0.85f, 1.00f);

    // ---- 1. Scan lines ----
    bool        scanEnabled        = true;
    bool        scanHarmonic       = false;       // add 3× harmonic stripe
    float       scanFreq           = 220.0f;      // stripes per screen height
    float       scanSpeed          = 0.5f;        // cycles per second
    float       scanIntensity      = 0.45f;       // 0..1 modulation depth

    // ---- 2. Fresnel rim ----
    bool        fresnelEnabled     = true;
    glm::vec3   fresnelColor       = glm::vec3(0.40f, 0.85f, 1.00f);
    float       fresnelPower       = 5.0f;        // 0.5..8.0
    float       fresnelIntensity   = 1.5f;        // 0..4

    // ---- 3. Edge flicker ----
    bool        flickerEnabled     = true;
    glm::vec3   flickerColor       = glm::vec3(1.0f, 1.0f, 1.0f);
    float       flickerRate        = 8.0f;        // Hz (buckets per second)
    float       flickerProb        = 0.10f;       // fraction of buckets that fire
    float       flickerDuration    = 0.20f;       // fraction of bucket

    // ---- 4. Vertical glitch bars ----
    bool        glitchEnabled      = false;
    bool        glitchRgbShift     = true;        // chroma-aberration during burst
    float       glitchRate         = 6.0f;        // Hz
    float       glitchBands        = 32.0f;       // horizontal bands per screen
    float       glitchProb         = 0.08f;       // fraction of bands active
    float       glitchAmplitude    = 0.025f;      // screen-normalized U shift

    // ---- 5. Wireframe overlay ----
    enum class WireMode : int {
        Off    = 0,
        Deriv  = 1,   // fwidth(worldPos) — works on any mesh
        Bary   = 2,   // requires baked barycentric buffer (3× verts)
    };
    WireMode    wireMode           = WireMode::Deriv;
    glm::vec3   wireColor          = glm::vec3(0.40f, 0.85f, 1.00f);
    float       wireThickness      = 1.2f;        // pixels
    float       wireSharpness      = 1.5f;
    float       wireIntensity      = 0.8f;

    // ---- 6. Color shift ----
    bool        hueEnabled         = false;
    float       hueSpeed           = 0.05f;       // radians per second
    float       hueOffset          = 0.0f;        // static rotation
    bool        chromaAberration   = false;
    float       caAmount           = 0.003f;      // UV offset per channel

    // ---- 7. Z-fade dissolve ----
    bool        dissolveEnabled    = false;
    bool        dissolveNoise      = false;
    int         dissolveAxis       = 2;           // 0=X, 1=Y, 2=Z (default vertical)
    float       dissolveOrigin     = 0.0f;        // start plane position (meters)
    float       dissolveSpeed      = 0.5f;        // m/s
    float       dissolveBand       = 0.5f;        // softness of leading edge (m)
    float       dissolveNoiseAmp   = 0.3f;        // noise contribution to t

    // ---- Master opacity modulator binding ----
    // Same pattern as StructureLayer::displaceModSlot. The effective opacity
    // fed to the shader is:
    //   saturate(this->opacity + mods->eval(opacityModSlot, t) * opacityModDepth)
    int         opacityModSlot     = 0;           // 0 = unbound
    float       opacityModDepth    = 0.5f;

    // ---- Computed helpers (called by StructureLayer) ----
    // Compose the effective mask bits from the boolean enables + mode enums.
    // The shader reads this single uint32 and branches on bits.
    uint32_t    effectMask() const;

    // Compute master effective opacity given the modulator bank.
    float       effectiveOpacity(double t, const class ModulatorBank* mods) const;
};

} // namespace spacegen
