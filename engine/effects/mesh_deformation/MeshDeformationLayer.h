#pragma once
// MeshDeformationLayer — Notch/Houdini-style chainable vertex deformer
// stack. Sits next to StructureLayer in the bus; when present, the
// MetalRenderer queries it for the active op list and packs the chain
// into the structure vertex shader's uniform buffer (see INTEGRATION.md
// in this folder).
//
// Up to kMaxOps operators applied in list order, each with its own
// enabled flag, intensity (with optional modulator-bank slot), and
// per-op params. The dispatcher lives in the vertex shader
// (deformers.metal.inc); the CPU side just packs uniforms.
//
// v1 ships 8 op types — see README.md for the math of each.

#include "../../core/Layer.h"
#include "../../core/ModulatorBank.h"

#include <glm/glm.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace spacegen {

// ----------------------------------------------------------------------------
// Op definition.
// ----------------------------------------------------------------------------

// IMPORTANT: keep this enum in sync with the OP_TYPE_* constants and
// the dispatcher switch in deformers.metal.inc.
enum class DeformOpType : int {
    None       = 0,   // dispatcher skips
    CurlNoise  = 1,
    Voronoi    = 2,
    Twist      = 3,
    Bend       = 4,
    Taper      = 5,
    Wave       = 6,
    Spherify   = 7,
    FFDLite    = 8,
};

// Maximum number of ops in the chain. GPU uniform array size — bump
// this and the corresponding constant in deformers.metal.inc together.
constexpr int kMaxDeformOps = 16;

// A single deformer in the chain.
//
// Per-op parameters are stored in two glm::vec4 slots `pA` and `pB`.
// The semantic layout depends on the type and is documented in
// deformers.metal.inc next to each function. The FFD op stores its 8
// corner deltas in `ffdCorners` (CPU side) and the renderer packs them
// into the uniform separately — they don't fit in two vec4s.
//
// One ModulatorBank slot drives `intensity`. Future revisions can add
// per-param mod slots.
struct DeformOp {
    DeformOpType type      = DeformOpType::None;
    bool         enabled   = true;
    float        intensity = 1.0f;          // 0..1 wet/dry
    glm::vec4    pA        = glm::vec4(0.0f);
    glm::vec4    pB        = glm::vec4(0.0f);

    // Optional global ModulatorBank binding on `intensity`.
    // slot 0 = unbound. Output of slot (normalized -1..+1) is
    // multiplied by `intensityModDepth` and added to `intensity`,
    // clamped to [0, 1] before being sent to the GPU.
    int   intensityModSlot  = 0;
    float intensityModDepth = 0.5f;

    // FFD-lite only: deltas from the 8 bbox corners (worldspace).
    // Ordered (i, j, k) ∈ {0, 1}^3 with bit packing i + 2j + 4k:
    //   0: (-,-,-), 1: (+,-,-), 2: (-,+,-), 3: (+,+,-),
    //   4: (-,-,+), 5: (+,-,+), 6: (-,+,+), 7: (+,+,+)
    // bbox itself is pA.xyz (min) and pB.xyz (max).
    std::array<glm::vec3, 8> ffdCorners{};

    // Display name (operator can rename per-op for clarity).
    char displayName[32] = {0};
};

// ----------------------------------------------------------------------------
// Layer.
// ----------------------------------------------------------------------------

class MeshDeformationLayer : public ILayer {
public:
    MeshDeformationLayer();

    LayerKind   kind()     const override { return LayerKind::Effect; }
    const char* typeName() const override { return "Mesh Deformation"; }
    void        render(RenderContext& /*ctx*/) override {}  // consumed by StructureLayer / MetalRenderer
    void        drawInspector() override;

    // ---- The op chain. Applied in list order. ----
    // Public for simplicity (v1 — see Layer.h commentary).
    std::vector<DeformOp> ops;

    // Add a new op of a given type at the end with sensible defaults.
    DeformOp& addOp(DeformOpType type);

    // Remove the op at `idx` (no-op if out of range).
    void removeOp(int idx);

    // Move the op at `from` to position `to` (clamped). Used by the
    // drag-reorder UI.
    void moveOp(int from, int to);

    // ---- Snapshot for the renderer. ----
    // Computes effective intensities (after modulator-bank evaluation)
    // and packs the chain into a flat structure ready to memcpy into
    // the shader uniform. `t` is current time in seconds, `mods` may
    // be null (no modulation applied).
    //
    // The structure mirrors the GPU layout exactly. `count` is the
    // number of active (enabled, intensity > 0) ops; the rest of the
    // array is type = OpType::None which the dispatcher skips.
    struct GpuChain {
        struct GpuOp {
            // .x = type as int, .y = effective intensity, .z, .w unused
            glm::vec4 header;
            glm::vec4 pA;
            glm::vec4 pB;
        };
        std::array<GpuOp, kMaxDeformOps> ops;
        int       count = 0;

        // FFD-lite uses a separate flat array of 8 vec4s per op. The
        // dispatcher reads from `ffdSlots[op.header.w]`. Most ops set
        // header.w = -1.0 (unused).
        //
        // We allow up to 4 FFD ops in the chain — enough for most
        // setups (one FFD is the usual case).
        static constexpr int kMaxFfdSlots = 4;
        std::array<std::array<glm::vec4, 8>, kMaxFfdSlots> ffdSlots{};
        int ffdCount = 0;
    };

    // Build a snapshot. Caller is responsible for memcpy'ing into the
    // shader uniform.
    GpuChain snapshot(double t, const ModulatorBank* mods) const;

private:
    // Internal ID counter for stable ImGui IDs across reorder.
    uint32_t nextOpId_ = 1;
    std::array<uint32_t, kMaxDeformOps> opIds_{};

    // Per-op inspector. Returns true if the op should be removed
    // (operator clicked the X). Sets *moveDir to -1 / +1 if the
    // operator clicked the up/down reorder buttons.
    bool drawOpInspector(int idx, int* moveDir);
};

} // namespace spacegen
