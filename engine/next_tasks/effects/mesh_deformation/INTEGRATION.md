# Integrating MeshDeformationLayer into MetalRenderer

This document is a step-by-step recipe to wire the new deformer stack
into the existing structure pass. **No CMake changes required** —
the new files build under the same `engine/core` glob (Layer header
already includes it).

## File placement after sign-off

Move the four files from this directory to:

    engine/core/MeshDeformationLayer.h
    engine/core/MeshDeformationLayer.cpp
    engine/backends/metal/shaders/deformers.metal.inc   (or inline in MetalRenderer.cpp)

The `.metal.inc` snippet can either be:
1. Embedded as a `R"MSL(...)MSL"` constant in `MetalRenderer.cpp` and
   concatenated with `kStructurePbrMSL` at runtime, **or**
2. Saved as a `.metal.inc` file in `engine/backends/metal/shaders/`
   and read with `std::ifstream` at boot.

Option 1 is simpler and what the existing code does — recommend that.

---

## Step 1: Replace the `fx` slot in `Uniforms`

In `MetalRenderer.cpp` find the `Uniforms` struct (around line 396).
Replace the single `glm::vec4 fx;` line with the new deformer block:

```cpp
struct GpuDeformOp {
    glm::vec4 header;   // .x type, .y intensity, .z time, .w ffd slot idx
    glm::vec4 pA;
    glm::vec4 pB;
};

struct Uniforms {
    // ... unchanged up to fx ...
    glm::vec4 deformParams;            // .x deformCount (int), .y ffdCount, .zw reserved
    GpuDeformOp deformers[16];         // kMaxDeformOps
    glm::vec4 ffdCorners[4 * 8];       // kMaxFfdSlots * 8 corner deltas
    // ... rest unchanged (syphonMixTint, syphonParams, dirs, spots) ...
};
```

(Remove the old `glm::vec4 fx;` field. The displace + twist behavior
is now represented as ops in the chain.)

Mirror the change in the MSL struct in `kStructurePbrMSL`:

```msl
struct Uniforms {
    // ... unchanged ...
    float4    deformParams;                  // .x opCount, .y ffdCount
    GpuDeformOp deformers[MAX_DEFORM_OPS];   // 16
    float4    ffdCorners[MAX_FFD_SLOTS * 8]; // 4 * 8 = 32 vec4s
    // ... rest unchanged ...
};
```

> **Memory budget.** This adds 16 ops × 48 bytes + 32 vec4s × 16 bytes
> + 16 bytes of params = 768 + 512 + 16 = **1296 bytes**. Trivial for
> a uniform buffer; total `Uniforms` stays well under 4 KB.

---

## Step 2: Paste `deformers.metal.inc` into `kStructurePbrMSL`

In `MetalRenderer.cpp`, find the line:

```cpp
constexpr const char* kStructurePbrMSL = R"MSL(
#include <metal_stdlib>
using namespace metal;
```

After the `MAX_SPOTS` / `MAX_DIRS` constants and before the existing
`vsHash` function, paste the entire contents of
`deformers.metal.inc`. The constants `OP_NONE` etc and the structs
must be visible to `vs_main`.

Remove the old `vsHash` function — `hash13` in the new snippet covers
the same purpose if anything still wants a per-vertex hash scalar.
Search for `vsHash` and confirm there are no other callers in the
shader.

---

## Step 3: Replace the displace + twist block in `vs_main`

In `kStructurePbrMSL`, locate the existing block:

```msl
// ---- Mesh-effect displacement (local space) ----
float3 pos = in.position;
float displaceAmount = u.fx.x;
float displaceScale  = u.fx.y;
float twistAmount    = u.fx.z;
if (twistAmount != 0.0) { ... }
if (displaceAmount != 0.0) { ... }
```

Replace **the whole block** with:

```msl
// ---- Vertex deformer chain (MeshDeformationLayer) ----
int opCount = int(u.deformParams.x + 0.5);
float3 pos = applyDeformerChain(in.position,
                                 in.normal,
                                 u.deformers,
                                 opCount,
                                 u.ffdCorners);
```

The downstream `float4 world = u.model * float4(pos, 1.0);` line stays
untouched.

> **Normal handling at v1.** The chain only deforms position. The
> original `in.normal` is still used for lighting (passed via
> `out.worldNormal`). This is the documented limitation in
> README.md — large displacements will produce slightly wrong specular
> highlights. Acceptable for projection-mapping use where the mesh is
> usually overlaid with projected video anyway.

---

## Step 4: Pack the uniform on the CPU side

In `MetalRenderer::renderStructureMeshes` find the block that packs
`u.fx`:

```cpp
const ModulatorBank* mods = ctx.scene ? &ctx.scene->modulators : nullptr;
float dispEff = layer.displaceAmount;
float twistEff = layer.twistAmount;
if (mods) {
    dispEff  += mods->eval(layer.displaceModSlot, t) * layer.displaceModDepth;
    twistEff += mods->eval(layer.twistModSlot,    t) * layer.twistModDepth;
}
u.fx = glm::vec4(dispEff, layer.displaceScale, twistEff, 0.0f);
```

Replace with:

```cpp
const ModulatorBank* mods = ctx.scene ? &ctx.scene->modulators : nullptr;

// Find an enabled MeshDeformationLayer on the bus, if any.
const MeshDeformationLayer* deform = nullptr;
if (ctx.scene) {
    for (auto& l : ctx.scene->bus.layers) {
        if (!l) continue;
        if (l->state != LayerState::Enabled) continue;
        if (auto* d = dynamic_cast<const MeshDeformationLayer*>(l.get())) {
            deform = d;
            break;  // first one wins (v1 — only one deformer per scene)
        }
    }
}

if (deform) {
    auto chain = deform->snapshot(ctx.elapsedSeconds, mods);
    u.deformParams = glm::vec4(static_cast<float>(chain.count),
                                  static_cast<float>(chain.ffdCount),
                                  0.0f, 0.0f);
    std::memcpy(u.deformers, chain.ops.data(),
                  sizeof(MeshDeformationLayer::GpuChain::GpuOp)
                  * kMaxDeformOps);
    // Flatten the 4×8 FFD corner array into the 32-slot uniform field.
    for (int s = 0; s < MeshDeformationLayer::GpuChain::kMaxFfdSlots; ++s) {
        for (int c = 0; c < 8; ++c) {
            u.ffdCorners[s * 8 + c] = chain.ffdSlots[s][c];
        }
    }
} else {
    u.deformParams = glm::vec4(0.0f);
    std::memset(u.deformers,   0, sizeof(u.deformers));
    std::memset(u.ffdCorners,  0, sizeof(u.ffdCorners));
}
```

Don't forget the `#include "../../core/MeshDeformationLayer.h"` at the
top of `MetalRenderer.cpp`.

---

## Step 5: Retire `StructureLayer`'s displace + twist fields

Once verified working, mark these as deprecated in
`StructureLayer.h`:

```cpp
// DEPRECATED: use a MeshDeformationLayer instead. Will be removed in
// the next major.
[[deprecated("Use MeshDeformationLayer")]] float displaceAmount = 0.0f;
[[deprecated("Use MeshDeformationLayer")]] float displaceScale  = 1.5f;
[[deprecated("Use MeshDeformationLayer")]] float twistAmount    = 0.0f;
[[deprecated("Use MeshDeformationLayer")]] int   displaceModSlot   = 0;
[[deprecated("Use MeshDeformationLayer")]] float displaceModDepth  = 1.0f;
[[deprecated("Use MeshDeformationLayer")]] int   twistModSlot      = 0;
[[deprecated("Use MeshDeformationLayer")]] float twistModDepth     = 0.5f;
```

And remove the "3D mesh effects" section from `StructureLayer::drawInspector`.
Add a hint:

```cpp
ImGui::TextDisabled("Add a Mesh Deformation layer to the rack for");
ImGui::TextDisabled("displace / twist / bend / curl noise / FFD / etc.");
```

A simple project-migration step on load can convert legacy projects:
if `displaceAmount != 0` or `twistAmount != 0`, auto-create a
`MeshDeformationLayer` with one Curl Noise + one Twist op pre-populated
with the legacy values. See `Scene::loadFromJson` for where to plug
this in.

---

## Step 6: Register the new layer type in the bus UI

Wherever the "Add layer" menu lives in the operator UI (likely
`AppUi.cpp` or similar), add an entry:

```cpp
if (ImGui::MenuItem("Mesh Deformation")) {
    scene.bus.add<MeshDeformationLayer>();
}
```

Suggested rack position: **above** StructureLayer (effects feed
the structure, which is the generator). The renderer doesn't care —
it does a `dynamic_cast` lookup in `renderStructureMeshes`.

---

## Verification checklist

- Build clean (no warnings about unused `vsHash`).
- Empty deformer layer → structure renders identical to v0 (no ops
  active).
- Add a Twist op with axis Z, amount 0.8 → same visual as legacy
  `twistAmount = 0.8` in StructureLayer.
- Add a Curl Noise op with amount 0.2 → see flowing, animated
  displacement; vertices should swirl over time.
- Add an FFD-lite op, drag one corner → see local trilinear warp.
- 8 ops simultaneously enabled on a 100k-tri mesh → frame time should
  stay <1ms vertex-bound on Apple M-series.
- Disable the deformer layer → structure snaps back to undeformed.
- Bind LFO 1 to a Wave op intensity → see the wave breathe.

---

## Future extensions (not in v1)

- **Per-parameter modulator binding.** Currently only `intensity` is
  modulatable. Adding a second `paramAModSlot` + `paramAModDepth`
  pair would let the operator LFO the wave frequency, bend angle, etc.
  Trivial CPU change, slightly larger uniform.
- **Normal recomputation.** A compute-shader prepass that rebuilds
  normals from neighbor positions would make lighting correct under
  large bends. ~200 LOC of additional MSL + a vertex-adjacency table
  on the mesh.
- **Per-op modulator on Wave.speed / Bend.angle / Twist.amount.** Same
  pattern as above but only for the parameter that "wants" animation
  per op.
- **Recipe presets** (Liquid, Statue, Banner, etc — see README) as
  one-click buttons in the inspector that populate the chain with the
  right ops.
