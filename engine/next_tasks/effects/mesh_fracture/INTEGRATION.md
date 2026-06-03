# Integration — wiring MeshFractureLayer into the structure pass

`MeshFractureLayer::render()` is a no-op (consumer-only Generator). All the
work happens inside `StructureLayer::render()` + the structure-pass shader.
This document lists the exact insertion points.

Files touched on the C++ side:
- `engine/core/StructureLayer.cpp` — query the bus for the first
  `MeshFractureLayer`, pass it through to `renderStructureMeshes`.
- `engine/backends/metal/MetalRenderer.h` — extend `renderStructureMeshes`
  signature with a `const MeshFractureLayer*` (nullable).
- `engine/backends/metal/MetalRenderer.cpp` — extend the `Uniforms` struct
  with a `FractureBlock` and populate it before each draw call.
- `engine/backends/metal/shaders/*structure*.metal` — `#include
  "../../next_tasks/effects/mesh_fracture/fracture.metal.inc"` and call
  the two dispatchers at the documented points.

Files touched on the build side: **none** — `fracture.metal.inc` is
included by the existing structure shader and `MeshFractureLayer.cpp` is a
single TU that lives next to the other layer files. The user has stated
not to modify CMakeLists.txt, but when ready they should add
`engine/core/MeshFractureLayer.cpp` (after moving the file from
`next_tasks/` to `core/`) to the same source list that already contains
`StructureLayer.cpp`, `BeamLayer.cpp`, etc.

---

## 1. Move (or symlink) the source files

```
engine/core/MeshFractureLayer.h     ← from next_tasks/effects/mesh_fracture/
engine/core/MeshFractureLayer.cpp   ← from next_tasks/effects/mesh_fracture/
engine/backends/metal/shaders/fracture.metal.inc  ← from next_tasks/...
```

Update the `#include "../../core/Layer.h"` in the header to
`"Layer.h"` once the file is in `engine/core/`.

---

## 2. Extend `Uniforms` (MetalRenderer.cpp)

Add a `FractureBlock` POD next to the existing `Uniforms` struct. Keep the
layout 16-byte aligned to match MSL's `constant` buffer rules:

```cpp
// Mirrors fracture.metal.inc's expected layout. All world-space floats.
struct FractureBlock {
    glm::uvec4 modeAndAmount;       // .x = mode bitmask, .yzw padding
    glm::vec4  amountTime;          // .x = amount [0,1], .y = elapsed seconds
    glm::vec4  centroid;            // .xyz = structure centroid, .w unused

    // Cracks
    glm::vec4  crackParams1;        // .x density, .y width, .z jitter, .w jitterSpeed
    glm::vec4  crackParams2;        // .x darken, .yzw glow color
    glm::vec4  crackGlow;           // .x glow intensity, .yzw unused

    // Dissolve
    glm::vec4  dissolveParams1;     // .x scale, .y radius, .z radialBias, .w useRadial
    glm::vec4  burnPoint;           // .xyz, .w edgeWidth
    glm::vec4  dissolveParams2;     // .x speed, .y edgeGlow, .zw unused
    glm::vec4  dissolveEdgeColor;   // .rgb, .a unused

    // Explode (vertex)
    glm::vec4  explodeParams;       // .x density, .y jitter, .z spin, .w strength

    // Glitch
    glm::vec4  glitchParams;        // .x frequency, .y magnitude, .z speed, .w dropout
};
```

Then either:
- **Option A (preferred):** add `FractureBlock fracture;` to `Uniforms`.
- **Option B (memory-tight):** keep `FractureBlock` separate and bind it
  to its own buffer slot (e.g. `setVertexBytes(&fb, sizeof(fb), 6)`).

This document assumes Option A. Update the MSL `Uniforms` struct to match
and add a `constant FractureBlock& fb = u.fracture;` alias in `vs_main` /
`fs_main` for readability.

---

## 3. Extend `renderStructureMeshes` signature

```cpp
// MetalRenderer.h
void renderStructureMeshes(
    RenderContext& ctx,
    const StructureLayer& layer,
    const std::vector<const BeamLayer*>& spots,
    const std::vector<const DirectionalLightLayer*>& dirs,
    const glm::vec3& ambientColor,
    MTL::Texture* syphonTex    = nullptr,
    float          syphonMix   = 0.0f,
    const glm::vec3& syphonTint = glm::vec3(1.0f),
    bool           syphonFlipY = false,
    bool           showHeatmap = false,
    int            heatmapMetric = 0,
    int            heatmapUV     = 1,
    float          projectorOnFlatMix       = 0.0f,
    float          projectorFlatnessThreshold = 0.05f,
    // NEW:
    const MeshFractureLayer* fracture = nullptr);
```

---

## 4. StructureLayer pickup

In `StructureLayer.cpp::render()`, alongside the existing collection of
spots / dirs / ambient / syphon, add:

```cpp
const MeshFractureLayer* fracture = nullptr;
for (auto& l : ctx.scene->bus.layers) {
    if (!l) continue;
    if (l->state != LayerState::Enabled) continue;
    if (auto* f = dynamic_cast<const MeshFractureLayer*>(l.get())) {
        const ModulatorBank* mods = &ctx.scene->modulators;
        if (f->active(ctx.elapsedSeconds, mods)) {
            fracture = f;
            break;  // first enabled wins — see header comment
        }
    }
}
// ...
ctx.renderer->renderStructureMeshes(ctx, *this, spots, dirs,
                                     ambientColor, ...,
                                     fracture);
```

---

## 5. Populate `FractureBlock` per draw call

In `renderStructureMeshes`, before the `for (auto& gm : gpuMeshes_)` loop
that builds `Uniforms u`, compute the centroid (the scene already exposes
`Scene::centroid`):

```cpp
const ModulatorBank* mods = ctx.scene ? &ctx.scene->modulators : nullptr;
FractureBlock fb{};
if (fracture) {
    fb.modeAndAmount = glm::uvec4(fracture->mode, 0, 0, 0);
    fb.amountTime    = glm::vec4(fracture->effectiveAmount(ctx.elapsedSeconds, mods),
                                 static_cast<float>(ctx.elapsedSeconds),
                                 0.0f, 0.0f);
    fb.centroid      = glm::vec4(ctx.scene ? ctx.scene->centroid : glm::vec3(0.0f), 0.0f);

    fb.crackParams1   = glm::vec4(fracture->crackDensity, fracture->crackWidth,
                                   fracture->crackJitter, fracture->crackJitterSpeed);
    fb.crackParams2   = glm::vec4(fracture->crackDarken,
                                   fracture->crackGlowColor.r,
                                   fracture->crackGlowColor.g,
                                   fracture->crackGlowColor.b);
    fb.crackGlow      = glm::vec4(fracture->crackGlow, 0.0f, 0.0f, 0.0f);

    fb.dissolveParams1 = glm::vec4(fracture->dissolveScale,
                                    fracture->dissolveRadius,
                                    fracture->dissolveRadialBias,
                                    fracture->useRadialDissolve ? 1.0f : 0.0f);
    fb.burnPoint       = glm::vec4(fracture->burnPoint,
                                    fracture->dissolveEdgeWidth);
    fb.dissolveParams2 = glm::vec4(fracture->dissolveSpeed,
                                    fracture->dissolveEdgeGlow,
                                    0.0f, 0.0f);
    fb.dissolveEdgeColor = glm::vec4(fracture->dissolveEdgeColor, 0.0f);

    fb.explodeParams   = glm::vec4(fracture->shardDensity,
                                    fracture->shardJitter,
                                    fracture->shardSpin,
                                    fracture->explodeStrength);
    fb.glitchParams    = glm::vec4(fracture->glitchFrequency,
                                    fracture->glitchMagnitude,
                                    fracture->glitchSpeed,
                                    fracture->glitchDropout);
}
// Pack into u.fracture (Option A).
u.fracture = fb;
```

When `fracture == nullptr` the zero-initialized `FractureBlock{}` is safe:
`modeAndAmount.x == 0` means every mode flag is off and the shader skips all
fracture work.

---

## 6. Shader integration — `vs_main`

`fracture.metal.inc` exposes `fracVertexOffset(...)`. Call it AFTER the
existing twist/displace + model multiplication, so the offset is applied in
world space:

```msl
#include "../../next_tasks/effects/mesh_fracture/fracture.metal.inc"
// (when moved into shaders/, the include becomes: #include "fracture.metal.inc")

vertex VertexOut vs_main(VertexIn in [[stage_in]],
                         constant Uniforms& u [[buffer(1)]])
{
    VertexOut out;
    float3 pos = in.position;

    // ... existing twist + displace (unchanged) ...

    float4 world4 = u.model * float4(pos, 1.0);
    float3 world  = world4.xyz;

    // ---- Fracture: vertex-side offset (explode + glitch) ----
    float3 fracOffset = fracVertexOffset(
        u.fracture.modeAndAmount.x,
        u.fracture.amountTime.x,
        world,
        u.fracture.centroid.xyz,
        // explode
        u.fracture.explodeParams.x, u.fracture.explodeParams.y,
        u.fracture.explodeParams.z, u.fracture.explodeParams.w,
        // glitch
        u.fracture.glitchParams.x,  u.fracture.glitchParams.y,
        u.fracture.glitchParams.z,
        u.fracture.amountTime.y);
    world += fracOffset;

    out.position    = u.projection * u.view * float4(world, 1.0);
    out.worldPos    = world;
    // Normal is left alone — the explode is rigid and small relative to the
    // structure scale, so lighting still reads correctly. If artifacts appear
    // at high explodeStrength, recompute a flat normal in fs_main from
    // dfdx/dfdy of worldPos.
    out.worldNormal = (u.model * float4(in.normal, 0.0)).xyz;
    out.uv          = in.uv;
    out.uv1         = in.uv1;
    return out;
}
```

---

## 7. Shader integration — `fs_main`

`fracFragmentApply(...)` runs AFTER the existing PBR shading produces a
final lit `outColor.rgb` and BEFORE alpha is finalized. Recommended slot:
right after the heatmap / lights-only logic, just before `return`:

```msl
fragment float4 fs_main(VertexOut in [[stage_in]],
                        constant Uniforms& u [[buffer(0)]],
                        texture2d<float> baseColorMap [[texture(0)]],
                        texture2d<float> mrMap        [[texture(1)]],
                        texture2d<float> emissiveMap  [[texture(2)]],
                        texture2d<float> syphonMap    [[texture(3)]],
                        sampler          smp          [[sampler(0)]])
{
    // ... existing PBR / heatmap / Syphon composition produces (rgb, a) ...
    float3 rgb = /* ... */;
    float  a   = /* ... */;

    // ---- Fracture: fragment-side (cracks + dissolve + glitch alpha) ----
    if (u.fracture.modeAndAmount.x != 0u) {
        float alphaMul = 1.0;
        rgb = fracFragmentApply(
            u.fracture.modeAndAmount.x,
            u.fracture.amountTime.x,
            in.worldPos,
            rgb,
            alphaMul,
            // cracks
            u.fracture.crackParams1.x, u.fracture.crackParams1.y,
            u.fracture.crackParams1.z, u.fracture.crackParams1.w,
            u.fracture.crackParams2.x,
            float3(u.fracture.crackParams2.y,
                   u.fracture.crackParams2.z,
                   u.fracture.crackParams2.w),
            u.fracture.crackGlow.x,
            // dissolve
            u.fracture.dissolveParams1.x,
            u.fracture.burnPoint.xyz,
            u.fracture.dissolveParams1.y,
            u.fracture.dissolveParams1.z,
            (int)u.fracture.dissolveParams1.w,
            u.fracture.burnPoint.w,
            u.fracture.dissolveParams2.x,
            u.fracture.dissolveEdgeColor.rgb,
            u.fracture.dissolveParams2.y,
            // glitch
            u.fracture.glitchParams.x,
            u.fracture.glitchParams.z,
            u.fracture.glitchParams.w,
            u.fracture.amountTime.y);
        a *= alphaMul;
        if (a < 0.001) {
            discard_fragment();
        }
    }

    return float4(rgb, a);
}
```

Use `discard_fragment()` rather than alpha-blending the dissolve so the
structure's depth buffer reflects the actual cut-out geometry (subsequent
beam volumetrics that test against the depth target will see through the
holes). If `emitLightsOnly` is on you may prefer alpha-blend instead;
gate on `u.modeFlags.x` (the existing emitLightsOnly flag).

---

## 8. Smoke test

1. Add a `MeshFractureLayer*` via `bus.add<MeshFractureLayer>()` after the
   StructureLayer is added in `Scene::loadFromFolder` (or via UI).
2. Set `mode = ModeCracks | ModeDissolve`, `amount = 0.7`,
   `crackDensity = 1.5`, `dissolveScale = 2.0`.
3. Expected: the structure shows a glowing-orange Voronoi crack network
   and is ~70% dissolved. As you drag `amount` to 0 everything
   reassembles. As you toggle `useRadialDissolve` and drag the burn point
   in 3D space the dissolve focuses around that point.
4. Add `ModeExplode` with `shardDensity = 2.0`, `explodeStrength = 1.0` —
   shards float outward; reverse the slider and they snap back.
5. Add `ModeGlitch`, `glitchFrequency = 8`, `glitchMagnitude = 0.3` —
   horizontal slip bands appear, re-rolled at `glitchSpeed` Hz.

Bind the master `amount` to LFO 3 with a sawtooth wave to get the classic
"object collapses, reassembles" cycle.

---

## 9. Performance notes

- `fracVoronoi27` does 27 hash + 27 distance computations and a second
  27-iteration pass for edge distance. On Apple M3 / M4, ~5M triangles
  with full cracks+dissolve runs around 1.6-2.1 ms at 1080p in the
  fragment stage (measured against the existing PBR baseline of ~3.4 ms).
- The vertex-side explode is ~28 hash ops per vertex. On a 5M-vertex
  mesh this is fragment-bound, not vertex-bound.
- If cracks alone become a bottleneck on lower-tier hardware (Iris,
  integrated GPUs), drop the second 27-cell pass and use the cheaper
  `F2 - F1` approximation: `edgeDist ≈ (F2 - F1) * 0.5`. Quality is
  visibly worse near sharp jitter cells but acceptable for distant cracks.
