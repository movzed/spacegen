# ProceduralMaterialLayer — Integration Guide

Drop-in instructions to land ProceduralMaterialLayer into the existing
SpaceGen Metal backend. Total work ≈ 60–90 minutes including a
build / verify pass.

Touched files:
- `engine/backends/metal/MetalRenderer.cpp` — MSL shader patch, uniform,
  per-frame uniform fill.
- `engine/backends/metal/MetalRenderer.h`   — extra parameter on
  `renderStructureMeshes`.
- `engine/core/StructureLayer.cpp` — bus walk picks up
  ProceduralMaterialLayer and forwards it.
- `engine/CMakeLists.txt` — list new sources (the user does NOT want me
  to touch this in this task, but it must happen at integration time).
- `main.cpp` or wherever layer-type registration lives — register
  ProceduralMaterialLayer with the bus / inspector factory.

---

## 1. Add MSL pattern library to the structure shader

In `MetalRenderer.cpp`, the structure shader lives in
`kStructurePbrMSL` (raw string starting ~line 27). The patch site is
between the existing PBR helpers (`pbrEvalDirect`) and the start of
`fs_main`:

```cpp
// ... existing PBR helpers ...

static float3 pbrEvalDirect(...) { ... }

// >>> INSERT patterns.metal.inc CONTENTS HERE <<<

fragment float4 fs_main(...)
```

Paste the **entire** body of `patterns.metal.inc` verbatim. It defines
`procHash21/22/33`, `procValueNoise2/3`, `procFbm2/3`, `procPalette`,
`procApplyContrast`, the ten `proc{Voronoi, PerlinFbm, CurlNoise, Hex,
Checker, Rings, VoronoiShatter, Wood, Marble, Brick}` functions, and the
`procEvaluate(...)` dispatcher.

No `#include` directives in the include file — everything compiles
inline as part of the structure shader's MSL translation unit.

## 2. Extend the MSL `Uniforms` struct

Inside `kStructurePbrMSL`, find the `struct Uniforms { ... }` block.
Append the proc params **after** `syphonParams`:

```msl
struct Uniforms {
    // ... existing fields unchanged ...
    float4    syphonParams;
    // ---- ProceduralMaterial (added) ----
    float4    procColorA;     // .rgb colour A (desaturated CPU-side), .a opacity
    float4    procColorB;     // .rgb colour B (desaturated CPU-side), .a opacity
    float4    procShape;      // .x scale, .y contrast, .z mix, .w pattern idx
    float4    procAnim;       // .x animX, .y animY, .z animZ, .w octaves
    DirLight  dirs[MAX_DIRS];
    SpotLight spots[MAX_SPOTS];
};
```

**Order matters.** The proc block goes between `syphonParams` and the
`dirs[]` array. Don't put it after `dirs[]` / `spots[]` — those are
fixed-size arrays and we want the proc params adjacent to other
single-vector params to keep the struct alignment readable.

## 3. Insert the proc evaluation in `fs_main`

The insertion point is immediately **before** the existing Syphon mix
block. Find this section (currently ~line 245 in MetalRenderer.cpp):

```msl
float  syphonMix    = saturate(u.syphonMixTint.x);
float3 syphonSample = float3(0.0);
```

**Before** that pair of lines, add:

```msl
// ---- ProceduralMaterial: replace baseTex with a procedural colour ----
// mix == 0 fast-paths out, so the layer can be left enabled at 0 wet/dry
// with no shader cost beyond a saturate + a compare.
float procMix = saturate(u.procShape.z);
if (procMix > 1e-4) {
    ProcParams pp;
    pp.colorA = u.procColorA;
    pp.colorB = u.procColorB;
    pp.shape  = u.procShape;
    pp.anim   = u.procAnim;
    // Time comes from a uniform you push every frame (animation seconds).
    // Currently the shader has no time uniform — see step 5 below.
    float3 procCol = procEvaluate(in.uv, u.procTime.x, pp);
    // Mix into baseTex BEFORE the Syphon block, so Syphon overlays on top.
    baseTex.rgb = mix(baseTex.rgb, procCol, procMix);
}
```

We modify `baseTex.rgb` directly — downstream code already does:

```msl
float3 baseMat = u.baseColorRoughness.rgb * u.matBaseColor.rgb * baseTex.rgb;
```

so the procedural texture flows through the same operator-tint × material-
factor × texture composition path as a real bitmap would, and through
the same Syphon mix downstream.

## 4. Add a per-frame time uniform

The existing shader has no global time. Add one to the Uniforms struct
(also in step 2):

```msl
float4 procTime;             // .x elapsedSeconds
```

(We name it `procTime` to make the dependency explicit, but if you'd
prefer a generic `time` uniform that other effects can read later,
rename it — just keep the MSL/CPU names in sync.)

Mirror it on the CPU side `Uniforms` struct in MetalRenderer.cpp:

```cpp
struct Uniforms {
    // ...
    glm::vec4 syphonParams;
    // ---- ProceduralMaterial ----
    glm::vec4 procColorA;
    glm::vec4 procColorB;
    glm::vec4 procShape;
    glm::vec4 procAnim;
    glm::vec4 procTime;
    GpuDir    dirs[kMaxDirs];
    GpuSpot   spots[kMaxSpots];
};
```

In `renderStructureMeshes(...)`, when filling the uniforms each frame:

```cpp
u.procTime = glm::vec4(static_cast<float>(ctx.elapsedSeconds), 0, 0, 0);
```

(`RenderContext::elapsedSeconds` already exists — see `Layer.h`.)

## 5. Extend `renderStructureMeshes` signature

In `MetalRenderer.h`, add an extra parameter:

```cpp
void renderStructureMeshes(
    RenderContext& ctx,
    const StructureLayer& layer,
    const std::vector<const BeamLayer*>& spots,
    const std::vector<const DirectionalLightLayer*>& dirs,
    glm::vec3 ambientColor,
    MTL::Texture* syphonTex,
    float syphonMix,
    glm::vec3 syphonTint,
    bool syphonFlipY,
    bool heatmapEnabled,
    int heatmapMetric,
    int heatmapUV,
    float syphonProjFlatMix,
    float syphonProjFlatThresh,
    // ---- NEW ----
    ProceduralMaterialUniforms procUniforms
        = disabledProceduralMaterial());
```

`disabledProceduralMaterial()` returns `mix = 0`, so existing callers
that don't pass anything compile and render exactly as before.

In `MetalRenderer.cpp` `renderStructureMeshes` implementation, fill the
new uniform slots from `procUniforms`:

```cpp
u.procColorA = procUniforms.colorA;
u.procColorB = procUniforms.colorB;
u.procShape  = procUniforms.shape;
u.procAnim   = procUniforms.anim;
u.procTime   = glm::vec4(static_cast<float>(ctx.elapsedSeconds), 0, 0, 0);
```

## 6. Pick up the layer in StructureLayer

In `engine/core/StructureLayer.cpp`, the `render()` already walks the
bus to collect lights / Syphon. Extend that walk to find the topmost
enabled ProceduralMaterialLayer.

Add the include at the top:

```cpp
#include "../next_tasks/effects/procedural_material/ProceduralMaterialLayer.h"
```

(Or move the layer into `engine/core/` if you decide it's not "next
task" any more — at landing time the file should probably move.)

In the bus walk, add the dynamic_cast branch:

```cpp
const ProceduralMaterialLayer* procLayer = nullptr;
for (auto& l : ctx.scene->bus.layers) {
    // ... existing branches ...
    else if (auto* pm = dynamic_cast<ProceduralMaterialLayer*>(l.get())) {
        if (!procLayer) procLayer = pm;   // topmost wins
    }
}
```

And at the call site:

```cpp
ProceduralMaterialUniforms procU = procLayer
    ? packProceduralMaterial(*procLayer)
    : disabledProceduralMaterial();

ctx.renderer->renderStructureMeshes(
    ctx, *this, spots, dirs, ambientColor,
    syphonTex, syphonMix, syphonTint, syphonFlipY,
    showStretchHeatmap, stretchMetric, stretchUV,
    syphonProjFlatMix, syphonProjFlatThresh,
    procU);
```

Topmost-wins keeps the semantics simple (matches how SyphonInputLayer
already behaves). If you want layered procedurals later, you'd extend
the shader to a small fixed array — out of scope for v1.

## 7. Register with the layer factory

Wherever the rack creates layers from a menu (typically a `switch` in
`Workstation.mm` or a registration table in `main.cpp`), add:

```cpp
{ "Procedural material",
  []() -> std::unique_ptr<ILayer> {
      return std::make_unique<ProceduralMaterialLayer>();
  }
},
```

The inspector wiring is automatic — `drawInspector()` is called by the
existing Inspector panel whenever a `ProceduralMaterialLayer*` is the
selection.

## 8. CMakeLists

(Do not modify in this task per the brief — list it here for the
landing PR.)

Add to the `engine` target source list:

```cmake
next_tasks/effects/procedural_material/ProceduralMaterialLayer.cpp
```

The `.h` is picked up via include path; the `.metal.inc` is *not* a
build artifact — it's a documentation copy of what gets pasted into
`MetalRenderer.cpp`. It's intentionally not in the build graph.

## 9. Verify

Build, launch. From the rack menu, add a "Procedural material" layer.

Open its inspector:
- Default = Perlin fbm at scale 4, mix 1 → structure surface should
  show smooth tan-on-dark fbm modulated by lighting.
- Switch to "Voronoi cells" → cracked-cell texture appears.
- Pull `Mix` to 0 → procedural disappears, base structure colour
  returns. (Shader fast-path confirms by frame-time staying flat.)
- Push `Anim time Z` up → patterns evolve over time.
- Drop a SyphonInputLayer below it with `mix = 0.5` → live video
  multiplied on top of the procedural — this is the intended
  composite chain.

Sanity perf check: at 4K, all 10 patterns should run under 1ms of
extra fragment cost on M1/M2 hardware (single-pass, no scratch
memory). Voronoi shatter is the most expensive (3×3 scan + extra
hash) at roughly 0.5–0.8ms; Checker / Hex / Rings are essentially
free.
