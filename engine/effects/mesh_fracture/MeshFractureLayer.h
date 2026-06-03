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

    // ---- Built-in smooth animation -----------------------------------------
    // So the fracture animates on its own without the operator wiring an
    // external modulator. The envelope is computed INSIDE effectiveAmount()
    // and *added* to the static amount + modulator-bank contribution, then
    // clamped to [0,1]. Everything stays folded into that one resolved value
    // the renderer reads — no new GPU-facing fields.
    //
    //   Static  — no envelope; behaves exactly like the old layer.
    //   Pulse   — smooth cosine breathing between [animFloor .. animFloor+depth].
    //   Loop    — repeating attack -> hold -> decay ramp, eased (smoothstep).
    //   OneShot — a single eased attack -> hold -> decay, fired by retrigger()
    //             (or auto-looped every animPeriod seconds if animLoopOneShot).
    enum class AnimMode : uint32_t { Static, Pulse, Loop, OneShot };
    AnimMode   animMode    = AnimMode::Static;
    float      animDepth   = 1.0f;   // peak height the envelope adds [0..1]
    float      animFloor   = 0.0f;   // baseline the envelope sits on  [0..1]
    float      animRateHz  = 0.25f;  // Pulse breaths/sec; Loop cycles/sec
    float      animAttack   = 0.30f; // OneShot/Loop rise time (s)
    float      animHold     = 0.20f; // OneShot/Loop sustain at peak (s)
    float      animDecay    = 0.60f; // OneShot/Loop fall time (s)
    bool       animLoopOneShot = false; // auto-refire OneShot every animPeriod
    float      animPeriod   = 4.0f;  // s between auto OneShot fires

    // OneShot trigger bookkeeping. `retrigger()` stamps the next start time;
    // a negative start means "armed but never fired" (envelope reads 0).
    // mutable so it can be (re)armed lazily from the const effectiveAmount().
    mutable double animTriggerTime = -1.0;

    // Arm a OneShot envelope to begin at time `t` (seconds). Call from the
    // inspector button or any controller. Safe to call every frame.
    void retrigger(double t) const { animTriggerTime = t; }

    // ---- Cracks -------------------------------------------------------------
    // Voronoi cell network in world space; fragments near a cell edge darken
    // and/or emit a glow. `crackDensity` = cells per meter.
    float      crackDensity     = 1.8f;     // 0.2 .. 8.0
    float      crackWidth       = 0.06f;    // edge band thickness, normalized
    float      crackJitter      = 0.7f;     // cell jitter [0..1], 0 = cubic grid
    float      crackJitterSpeed = 0.12f;    // cell drift over time
    float      crackDarken      = 0.78f;    // 0..1 base-color multiplier at edge
    glm::vec3  crackGlowColor   = glm::vec3(1.00f, 0.52f, 0.18f); // warm ember
    float      crackGlow        = 1.1f;

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
    float      shardDensity = 2.2f;         // cells per meter (clamped 0.3..5.0)
    float      shardJitter  = 0.45f;        // direction randomness per shard
    float      shardSpin    = 0.5f;         // rotation around shard center
    // explodeStrength: meters of outward push at amount=1. The renderer caps
    // the outward push at 0.5m, so values much above ~0.5 just saturate; we
    // keep the default in the readable-shards range rather than fighting it.
    float      explodeStrength = 0.45f;

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

    // ---- Movement presets ---------------------------------------------------
    // One-call recipes that set the mode bitmask + the relevant per-mode params
    // + AnimMode/envelope for a good-looking, self-animating result. These only
    // touch existing fields, so the GPU contract is untouched. `Custom` is a
    // sentinel for "the operator has hand-tweaked it" and applyPreset() ignores
    // it.
    enum class Preset : int {
        Custom = 0,
        SlowDissolve,   // gentle radial burn, warm edge, slow Loop
        CrackPulse,     // ember crack network breathing on a Pulse
        ExplodeBurst,   // shards part once on a OneShot, eased
        GlitchStorm,    // fast datamosh Pulse
    };
    Preset currentPreset = Preset::Custom;
    void   applyPreset(Preset p, double now = 0.0);
};

} // namespace spacegen
