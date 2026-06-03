# HologramMaterialLayer — Integration Guide

This document tells a SpaceGen engine maintainer exactly **what to change**
in `MetalRenderer.cpp`, `StructureLayer.{h,cpp}`, and the build system to
land `HologramMaterialLayer`. Code goes in
`engine/next_tasks/effects/hologram/`; integration is mechanical paste +
field adds. No new pipelines, no new draw calls.

CMakeLists are **out of scope for this task** (the original spec said not
to touch them); when ready, the maintainer adds:

```
core/HologramMaterialLayer.{h,cpp}
```

to the existing `engine_core` source list.

---

## 1. Header / class plumbing

### 1.1 Move the files

```
engine/next_tasks/effects/hologram/HologramMaterialLayer.h
engine/next_tasks/effects/hologram/HologramMaterialLayer.cpp
```

go to

```
engine/core/HologramMaterialLayer.h
engine/core/HologramMaterialLayer.cpp
```

(same directory as `BeamLayer`, `AmbientLightLayer`, etc.).

### 1.2 No `Scene` change required

The layer is auto-discovered the same way Beam / Directional / Ambient
are: `StructureLayer::render()` scans `ctx.scene->bus.layers` via
`dynamic_cast`. No constructor wiring, no registry — just `bus.add<
HologramMaterialLayer>()` from the operator UI or the test harness.

---

## 2. StructureLayer changes

Two small edits.

### 2.1 `StructureLayer.cpp` — discover the hologram on the bus

Inside `StructureLayer::render`, where the existing code does

```cpp
} else if (auto* a = dynamic_cast<const AmbientLightLayer*>(l.get())) {
    ambientColor += a->color * (a->intensity * a->opacity);
} else if (auto* s = dynamic_cast<SyphonInputLayer*>(l.get())) {
    ...
}
```

add a sibling branch **before** the closing brace of the loop:

```cpp
} else if (auto* h = dynamic_cast<const HologramMaterialLayer*>(l.get())) {
    if (!holo) holo = h;   // first enabled hologram wins
}
```

with a `const HologramMaterialLayer* holo = nullptr;` declared next to the
existing `MTL::Texture* syphonTex = nullptr;`.

Pass `holo` through to `renderer->renderStructureMeshes(...)` (signature
extended with one new pointer at the end — see 2.2).

### 2.2 `MetalRenderer.h` — extend the renderStructureMeshes signature

Add one new trailing parameter:

```cpp
void renderStructureMeshes(
    RenderContext& ctx,
    const StructureLayer& layer,
    const std::vector<const BeamLayer*>& spots,
    const std::vector<const DirectionalLightLayer*>& dirs,
    const glm::vec3& ambientColor,
    MTL::Texture* syphonTex,
    float          syphonMix,
    const glm::vec3& syphonTint,
    bool           syphonFlipY,
    bool           showHeatmap,
    int            heatmapMetric,
    int            heatmapUV,
    float          projectorOnFlatMix,
    float          projectorFlatnessThreshold,
    const HologramMaterialLayer* hologram);   // <-- new (nullptr = no hologram)
```

Defaulting the new parameter to `nullptr` keeps existing call sites
compiling unchanged.

---

## 3. MSL shader changes (kStructurePbrMSL)

All changes live in `MetalRenderer.cpp`, inside the
`constexpr const char* kStructurePbrMSL = R"MSL(...)";` raw string. Paste
the four blocks from `hologram.metal.inc` at the indicated points.

### 3.1 Uniforms struct

Inside the MSL `struct Uniforms`, **append** at the bottom (before
`DirLight dirs[MAX_DIRS];`):

```msl
float4 holo[11];   // see HOLO_UNIFORMS in hologram.metal.inc
```

11 × `float4` = 176 B. Keep this contiguous after the existing scalar
fields so the struct stays 16-byte aligned.

### 3.2 Mask constants + helpers

Paste **[BEGIN HOLO_UNIFORMS]** (the `constant uint HMASK_*` block) and
**[BEGIN HOLO_HELPERS]** (hash, hue rotation, glitch shift, edge functions)
between the existing PBR helper functions and the `fragment fs_main`
declaration.

### 3.3 Early UV shift

Inside `fs_main`, **immediately after** the first two lines

```msl
float3 N = normalize(in.worldNormal);
float3 V = normalize(u.cameraWorldPos.xyz - in.worldPos);
```

paste **[BEGIN HOLO_EARLY_SHIFT]**. This computes `holoMask`, decodes the
opacity / elapsed / viewport-height fields, and applies the glitch UV
shift to `in.uv` and `in.uv1` before any texture sampling.

(`in.uv` and `in.uv1` are stage-in mutable copies — modifying them is
legal MSL.)

### 3.4 Optional chroma aberration on syphon

Find the existing line in the syphon sampling block:

```msl
float3 atlasSample = syphonMap.sample(smp, atlasUv).rgb;
```

Replace it with the **[BEGIN HOLO_CHROMA_SYPHON]** block (branches on
`HMASK_CHROMA_ABER`; falls through to the original single-tap when the
bit is clear).

### 3.5 Post-PBR hologram math

Find the existing end of `fs_main`, where the shader currently does:

```msl
float3 colorLin = ambient * baseColor + totalLight;
float3 colorOut = pow(max(colorLin, 0.0), float3(1.0 / 2.2));
return float4(colorOut, 1.0);
```

Insert **[BEGIN HOLO_POST_PBR]** between the `colorOut` assignment and
the `return`. The block reads `colorLin`, mutates `colorOut` in place, and
falls through to the existing return.

### 3.6 emitLightsOnly + hologram interaction

The existing shader has an **earlier** return for `emitLightsOnly`:

```msl
if (lightsOnly) {
    float lum   = dot(totalLight, ...);
    float alpha = saturate(lum);
    float3 colorOut = pow(max(totalLight, 0.0), float3(1.0 / 2.2));
    return float4(colorOut * alpha, alpha);
}
```

When both `emitLightsOnly` and the hologram are enabled, two reasonable
behaviors exist:

1. **Hologram wins**: hologram math runs on top of the lights-only result.
   Useful when compositing a hologram pass into Resolume on top of a plate.
2. **Lights-only wins**: hologram is suppressed in lights-only mode.

The spec'd behavior is **#1**. Implement it by moving the lights-only
return to AFTER `[BEGIN HOLO_POST_PBR]`, restructured as:

```msl
float3 colorLin;
float  alpha;

if (lightsOnly) {
    colorLin = totalLight;
    alpha    = saturate(dot(totalLight, float3(0.2126, 0.7152, 0.0722)));
} else {
    colorLin = ambient * baseColor + totalLight;
    alpha    = 1.0;
}
float3 colorOut = pow(max(colorLin, 0.0), float3(1.0 / 2.2));

// [HOLO_POST_PBR block here -- mutates colorOut]

if (lightsOnly) {
    return float4(colorOut * alpha, alpha);
}
return float4(colorOut, 1.0);
```

If the operator prefers behavior #2 (lights-only takes precedence), wrap
the `[HOLO_POST_PBR]` block in `if (!lightsOnly) { ... }` instead.

### 3.7 Optional: baked barycentric attribute

Only needed for `HMASK_WIRE_BARY` (the high-quality wireframe). Three
shader edits + one CPU-side mesh build step:

1. Add `float3 bary [[attribute(4)]];` to `VertexIn`.
2. Add `float3 bary;` to `VertexOut`.
3. In `vs_main`, add `out.bary = in.bary;` after `out.uv1 = in.uv1;`.

CPU side: implement
`MetalRenderer::buildBarycentricBuffer(size_t meshIdx)` that creates an
unindexed copy of `gpuMeshes_[meshIdx]` with 3 vertices per triangle and a
parallel barycentric buffer of `(1,0,0)/(0,1,0)/(0,0,1)`. Bind that
buffer to vertex slot 5 alongside the existing position/normal/uv/uv1
slots in `renderStructureMeshes`.

Until that buffer exists, the `Bary` wire mode falls back to `Deriv` in
the inspector (the layer surfaces the warning text). The shader treats
`HMASK_WIRE_BARY` without a bound buffer as `HMASK_WIRE_DERIV` because
`in.bary` will be the zero vertex attribute and `holo_edgeBary` would
return ~0 (and we OR in `HMASK_WIRE_DERIV` automatically in
`HologramMaterialLayer::effectMask()` when `wireMode == Bary` but the
buffer isn't ready). See "fallback" below.

---

## 4. CPU-side uniforms packing

Add the CPU mirror of the MSL `holo[11]` field at the bottom of the
existing `struct Uniforms` (the C++ one, ~line 396 in
`MetalRenderer.cpp`):

```cpp
glm::vec4 holo[11];
```

Then in `renderStructureMeshes`, **before** the existing per-mesh
`for (const auto& gm : gpuMeshes_)` loop, build the hologram pack once:

```cpp
glm::vec4 holoPacked[11]{};   // zero-init = HMASK_ENABLE clear = no-op
if (hologram) {
    const ModulatorBank* mods =
        ctx.scene ? &ctx.scene->modulators : nullptr;
    const float eff = hologram->effectiveOpacity(ctx.elapsedSeconds, mods);
    const uint32_t mask = (eff > 0.0f) ? hologram->effectMask() : 0u;

    const float vpH = static_cast<float>(ctx.height);
    holoPacked[0]  = glm::vec4(static_cast<float>(mask), eff,
                                static_cast<float>(ctx.elapsedSeconds),
                                vpH);
    holoPacked[1]  = glm::vec4(hologram->scanFreq,
                                hologram->scanSpeed,
                                hologram->scanIntensity, 0.0f);
    holoPacked[2]  = glm::vec4(hologram->fresnelColor,
                                hologram->fresnelPower);
    holoPacked[3]  = glm::vec4(hologram->fresnelIntensity,
                                hologram->flickerRate,
                                hologram->flickerProb,
                                hologram->flickerDuration);
    holoPacked[4]  = glm::vec4(hologram->flickerColor, 0.0f);
    holoPacked[5]  = glm::vec4(hologram->glitchRate,
                                hologram->glitchBands,
                                hologram->glitchProb,
                                hologram->glitchAmplitude);
    holoPacked[6]  = glm::vec4(hologram->wireColor,
                                hologram->wireThickness);
    holoPacked[7]  = glm::vec4(hologram->wireSharpness,
                                hologram->wireIntensity,
                                hologram->hueSpeed,
                                hologram->hueOffset);
    holoPacked[8]  = glm::vec4(hologram->caAmount,
                                hologram->dissolveOrigin,
                                hologram->dissolveSpeed,
                                hologram->dissolveBand);
    holoPacked[9]  = glm::vec4(static_cast<float>(hologram->dissolveAxis),
                                hologram->dissolveNoiseAmp,
                                hologram->masterGlowColor.r,
                                hologram->masterGlowColor.g);
    holoPacked[10] = glm::vec4(hologram->masterGlowColor.b,
                                0.0f, 0.0f, 0.0f);
}
```

Then, inside the per-mesh loop, after the existing
`u.syphonParams = ...;` line:

```cpp
std::memcpy(u.holo, holoPacked, sizeof(holoPacked));
```

The whole hologram pack is mesh-independent so we hoist the computation
out of the loop — only the `memcpy` runs per mesh.

---

## 5. Effect-mask "Bary fallback"

In `HologramMaterialLayer::effectMask()`, the `WireMode::Bary` case sets
`HMASK_WIRE_BARY`. To fall back gracefully when the barycentric buffer
isn't built, change the renderer code to OR in `HMASK_WIRE_DERIV` when no
`barycentricBuffer_` exists for the active mesh:

```cpp
if ((mask & HologramMaterialLayer::HMASK_WIRE_BARY) != 0u
    && !gm.barycentricBuffer) {
    mask &= ~HologramMaterialLayer::HMASK_WIRE_BARY;
    mask |=  HologramMaterialLayer::HMASK_WIRE_DERIV;
}
```

This way the inspector's "Baked bary" mode is a request that downgrades
silently to derivative mode until the buffer is built. The inspector
already shows the "Requires unindexed mesh" warning.

---

## 6. Smoke test

After landing the integration:

1. Create a `Scene` with a `StructureLayer` and one
   `HologramMaterialLayer` in the bus.
2. With `opacity = 0`, the structure renders identically to before
   (mask = 0, all branches collapse). Use this for a perf/no-regression
   baseline.
3. With `opacity = 1`, `scanEnabled` on, default values: structure shows
   scrolling horizontal lines at ~0.5 Hz.
4. Add `fresnelEnabled` on: cyan rim where surface faces away from camera.
5. Add `dissolveEnabled` on with `dissolveSpeed = 1.0`: a horizontal plane
   sweeps up over time and the structure materializes plane-by-plane.
6. Toggle `pureReplace`: lighting from spot/directional layers disappears,
   only hologram remains (good for "ghost mode" view).

---

## 7. File summary

| File                                   | Action                              |
| -------------------------------------- | ----------------------------------- |
| `core/HologramMaterialLayer.h`         | New (move from `next_tasks/...`).   |
| `core/HologramMaterialLayer.cpp`       | New (move from `next_tasks/...`).   |
| `core/StructureLayer.cpp`              | +1 dynamic_cast branch in render(). |
| `backends/metal/MetalRenderer.h`       | +1 param on renderStructureMeshes.  |
| `backends/metal/MetalRenderer.cpp`     | MSL paste-in (4 blocks) + CPU pack. |
| `backends/metal/MetalRenderer.cpp`     | Restructure final return (3.6).     |
| `CMakeLists.txt`                       | +2 source entries (out of scope).   |
