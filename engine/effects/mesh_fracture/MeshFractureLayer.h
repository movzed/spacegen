#pragma once
// MeshFractureLayer — recoverable cracks & breakage on the structure mesh.
//
// Like BeamLayer / DirectionalLightLayer / AmbientLightLayer, this layer's
// own render() is a no-op. StructureLayer::render() walks the bus, picks
// up the FIRST enabled MeshFractureLayer, and packs its params into the
// structure-pass uniforms (see INTEGRATION.md). The shader snippets in
// fracture.metal.inc consume those uniforms and produce all four effects.
//
// Why "first enabled" rather than "sum across all instances": fracture is a
// modal effect (you don't stack two dissolve burn points on the same mesh
// without artifacts). If the operator wants multi-fracture they can chain
// modes in one layer or switch via Solo. v2 may expose a stack.
//
// All parameters are pure POD; modulator-bank bindings follow the same
// pattern as StructureLayer::displaceModSlot / twistModSlot.

#include "../../core/Layer.h"

#include <cstdint>

#include <glm/glm.hpp>
#include <glm/vec3.hpp>

namespace spacegen {

class MeshFractureLayer : public ILayer {
public:
    MeshFractureLayer();

    LayerKind   kind()     const override { return LayerKind::Generator; }
    const char* typeName() const override { return "Mesh Fracture"; }

    // Consumed by StructureLayer — no direct render work.
    void        render(RenderContext& /*ctx*/) override {}
    void        drawInspector() override;

    // ---- Mode bitmask -------------------------------------------------------
    // Combine freely: e.g. Cracks | Dissolve to dissolve along an animated
    // crack network. The "Off" preset is `mode = 0`.
    enum Mode : uint32_t {
        ModeCracks   = 1u << 0,
        ModeDissolve = 1u << 1,
        ModeExplode  = 1u << 2,
        ModeGlitch   = 1u << 3,
    };
    uint32_t   mode = 0;

    // Master amount — multiplies every per-mode strength so the operator can
    // animate the whole fracture from 0 to 1 with a single slider / LFO.
    // Modulator-bank binding: slot 0 = unbound, 1..N = bank slot.
    float      amount         = 0.0f;
    int        amountModSlot  = 0;
    float      amountModDepth = 1.0f;

    // ---- Cracks -------------------------------------------------------------
    // Voronoi cell network in world space; fragments near a cell edge darken
    // and/or emit a glow. `crackDensity` = cells per meter.
    float      crackDensity     = 1.8f;     // 0.2 .. 8.0
    float      crackWidth       = 0.06f;    // edge band thickness, normalized
    float      crackJitter      = 0.7f;     // cell jitter [0..1], 0 = cubic grid
    float      crackJitterSpeed = 0.12f;    // cell drift over time
    float      crackDarken      = 0.85f;    // 0..1 base-color multiplier at edge
    glm::vec3  crackGlowColor   = glm::vec3(1.00f, 0.45f, 0.10f); // orange
    float      crackGlow        = 0.8f;

    // ---- Dissolve burn ------------------------------------------------------
    // Noise-driven discard, optionally radial from `burnPoint`. Edge of the
    // dissolve emits a glow.
    bool       useRadialDissolve = true;
    glm::vec3  burnPoint        = glm::vec3(0.0f);    // world-space center
    float      dissolveScale    = 2.4f;     // noise spatial frequency
    float      dissolveSpeed    = 0.0f;     // threshold rate (units of amount/sec); 0 = static
    float      dissolveRadius   = 12.0f;    // m, used as the radial normalization
    float      dissolveRadialBias = 0.5f;   // how strongly radius affects threshold
    float      dissolveEdgeWidth = 0.06f;
    float      dissolveEdgeGlow  = 1.4f;
    glm::vec3  dissolveEdgeColor = glm::vec3(1.00f, 0.55f, 0.15f);

    // ---- Per-shard explode --------------------------------------------------
    // Each vertex is assigned to a world-space grid cell at resolution
    // `shardDensity`. The vertex is offset outward along the cell's centroid
    // direction, with per-shard jitter and per-shard spin.
    float      shardDensity = 1.5f;         // cells per meter (clamped 0.3..5.0)
    float      shardJitter  = 0.7f;         // direction randomness per shard
    float      shardSpin    = 0.3f;         // rotation around shard center
    // explodeStrength: meters of outward push at amount=1.
    float      explodeStrength = 1.2f;

    // ---- Glitch tear --------------------------------------------------------
    // Vertical bands of vertices slide horizontally; per-fragment alpha
    // dropout adds the VHS / datamosh feel.
    float      glitchFrequency = 6.0f;      // bands per meter of world Y
    float      glitchMagnitude = 0.4f;      // m of lateral shift at amount=1
    float      glitchSpeed     = 8.0f;      // Hz — how often bands re-roll
    float      glitchDropout   = 0.15f;     // 0..1 fraction of fragments flicker-darkened

    // Resolved amount: master `amount` + the modulator-bank contribution.
    // Used by both the inspector readout and by StructureLayer when packing
    // uniforms. `mods` may be null (returns the static amount only).
    float effectiveAmount(double t, const class ModulatorBank* mods) const;

    // True if the layer would have any visible effect this frame given the
    // resolved amount. StructureLayer uses this to decide whether to populate
    // the fracture uniforms or write zeros.
    bool active(double t, const class ModulatorBank* mods) const;
};

} // namespace spacegen
