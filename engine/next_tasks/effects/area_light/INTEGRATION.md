# AreaLightLayer — Integration

This document lists the exact edits needed to wire `AreaLightLayer` into
the existing codebase. Five files touch GPU code; one file touches the
build. The data flow is identical to the other consumed-by-StructureLayer
lights (`BeamLayer`, `DirectionalLightLayer`, `AmbientLightLayer`):

```
AreaLightLayer (Generator, lives in Scene.bus)
     │
     │  StructureLayer::render() collects all enabled AreaLightLayer*
     ▼
StructureLayer::render() calls
     MetalRenderer::renderStructureMeshes(..., areas, ...)
     │
     ▼
MetalRenderer packs them into Uniforms::areas[MAX_AREAS] and
     dispatches the structure pipeline. The MSL fragment shader
     (kStructurePbrMSL) loops over u.areas[] (snippet in
     shader_snippet.metal.inc) and accumulates LoAreas.
```

---

## 1. CMakeLists.txt (engine/CMakeLists.txt)

Add the two new source files to the engine target. Locate the section
that lists `core/BeamLayer.cpp`, `core/DirectionalLightLayer.cpp`,
`core/AmbientLightLayer.cpp`, and add:

```cmake
core/AreaLightLayer.cpp
core/AreaLightLayer.h
```

(The headers are listed for IDE indexing only; the .cpp is the load-
bearing one.)

Move the two new files to `engine/core/` after review:

```
mv next_tasks/effects/area_light/AreaLightLayer.h   core/
mv next_tasks/effects/area_light/AreaLightLayer.cpp core/
```

The README, INTEGRATION, and shader_snippet stay in `next_tasks/effects/area_light/`
as docs.

---

## 2. engine/backends/metal/MetalRenderer.h

### 2a. Forward-declare `AreaLightLayer`

Right after the existing forward-decls (around line 21):

```cpp
class  AreaLightLayer;
```

### 2b. Change the signature of `renderStructureMeshes`

The function currently takes `spots` and `dirs`. Add a fourth light list,
**before** `ambientColor` so the cluster of light arguments stays
grouped:

```cpp
void renderStructureMeshes(
    RenderContext& ctx,
    const StructureLayer& layer,
    const std::vector<const BeamLayer*>& spots,
    const std::vector<const DirectionalLightLayer*>& dirs,
    const std::vector<const AreaLightLayer*>& areas,   // <-- NEW
    const glm::vec3& ambientColor,
    MTL::Texture* syphonTex    = nullptr,
    float          syphonMix   = 0.0f,
    const glm::vec3& syphonTint = glm::vec3(1.0f),
    bool           syphonFlipY = false,
    bool           showHeatmap = false,
    int            heatmapMetric = 0,
    int            heatmapUV     = 1,
    float          projectorOnFlatMix       = 0.0f,
    float          projectorFlatnessThreshold = 0.05f);
```

---

## 3. engine/backends/metal/MetalRenderer.cpp

### 3a. Include header

At the top of the file, alongside the other layer headers:

```cpp
#include "../../core/AreaLightLayer.h"
```

### 3b. Declare `kMaxAreas`

Next to the existing `kMaxSpots` / `kMaxDirs`:

```cpp
constexpr int kMaxAreas = 8;
```

### 3c. Add `AreaLight` MSL struct + array inside `kStructurePbrMSL`

Find the block defining `SpotLight` and `DirLight` (around line 50–60).
Add `MAX_AREAS`:

```msl
constant int   MAX_AREAS  = 8;
```

Add the `AreaLight` MSL struct (matches the C++ `GpuArea`):

```msl
struct AreaLight {
    float4 posIntensity;    // .xyz position, .w intensity
    float4 normRange;       // .xyz panel normal (unit), .w linear range
    float4 colorRadius;     // .rgb color, .a equivalent radius
};
```

Inside `struct Uniforms`, add the array (next to `spots[MAX_SPOTS]`):

```msl
AreaLight areas[MAX_AREAS];
```

### 3d. Repurpose `metallicCounts.y`

`metallicCounts` is currently `(x=metallic, z=spotCount, w=dirCount)`,
with `.y` unused. We use `.y` for `areaCount`. Update the MSL comment
on the field and the CPU-side packing site (see 3f).

### 3e. Splice the shader loop

Paste the contents of `shader_snippet.metal.inc` *inside* the
`fs_main` body, **immediately after** the spot-light loop ends
(after the closing `}` of `for (int i = 0; i < count; i++) { ... }`
that accumulates `LoSpots`, around line 362).

Then replace the line

```msl
float3 totalLight = LoDir + LoSpots + emission;
```

with

```msl
float3 totalLight = LoDir + LoSpots + LoAreas + emission;
```

### 3f. CPU-side `GpuArea` struct + `Uniforms` member

Next to the existing `GpuSpot` / `GpuDir` in the anonymous namespace
(around line 384):

```cpp
struct GpuArea {
    glm::vec4 posIntensity;
    glm::vec4 normRange;
    glm::vec4 colorRadius;
};
```

In the C++-side `struct Uniforms` (around line 396), after the
existing `GpuSpot spots[kMaxSpots]`, add:

```cpp
GpuArea areas[kMaxAreas];
```

### 3g. Pack area lights in `renderStructureMeshes`

After the existing block that packs `dirsPacked[]` (around line 866),
add the area-lights pack:

```cpp
// Pack area lights. AreaLightLayer::update() runs inside its own
// render(), so positionWorld/normalWorld are already fresh by the time
// StructureLayer::render() collects us. Same projection-mapping
// convention as spot lights: linear falloff inside `range`, one-sided
// panel test in the shader.
int areaCount = std::min(static_cast<int>(areas.size()), kMaxAreas);
GpuArea areasPacked[kMaxAreas]{};
for (int i = 0; i < areaCount; ++i) {
    const AreaLightLayer* a = areas[i];
    if (!a) continue;
    // Optionally also apply the intensityLFO offset here.
    float intenLfo = a->intensityLFO.eval(t);
    float inten    = std::max(0.0f, a->intensity + intenLfo) * a->opacity;
    areasPacked[i].posIntensity = glm::vec4(a->positionWorld, inten);
    areasPacked[i].normRange    = glm::vec4(a->normalWorld,   a->range);
    areasPacked[i].colorRadius  = glm::vec4(a->color,         a->effectiveR);
}
```

In the per-mesh loop, update `metallicCounts` to fill `.y`:

```cpp
u.metallicCounts = glm::vec4(layer.metallic,
                              static_cast<float>(areaCount),   // <-- .y
                              static_cast<float>(spotCount),
                              static_cast<float>(dirCount));
```

And copy the area array right after the existing two memcpys:

```cpp
std::memcpy(u.areas, areasPacked, sizeof(GpuArea) * kMaxAreas);
```

---

## 4. engine/core/StructureLayer.cpp

### 4a. Include header

```cpp
#include "AreaLightLayer.h"
```

### 4b. Collect area lights from the bus

Inside `StructureLayer::render()` (the loop over `ctx.scene->bus.layers`),
add a branch alongside `BeamLayer` / `DirectionalLightLayer`:

```cpp
std::vector<const AreaLightLayer*> areas;
// ...
} else if (auto* ar = dynamic_cast<const AreaLightLayer*>(l.get())) {
    areas.push_back(ar);
}
```

### 4c. Forward to MetalRenderer

Update the call site:

```cpp
ctx.renderer->renderStructureMeshes(ctx, *this, spots, dirs, areas,
                                     ambientColor,
                                     syphonTex, syphonMix, syphonTint,
                                     syphonFlipY,
                                     showStretchHeatmap,
                                     stretchMetric, stretchUV,
                                     syphonProjFlatMix,
                                     syphonProjFlatThresh);
```

---

## 5. engine/gui (optional but recommended) — wire it into the LayerAdd menu

Wherever the GUI offers "Add light layer" entries (look for usages of
`Bus::add<BeamLayer>()` in `engine/gui/`), add a parallel entry for
`AreaLightLayer`:

```cpp
if (ImGui::MenuItem("Area Light")) {
    scene.bus.add<AreaLightLayer>();
}
```

---

## 6. Smoke test

After integrating:

1. Build with `cmake --build build --target spacegen`.
2. Launch the engine, load any sample scene (e.g. the cube projection
   from `data/`).
3. From the layer rack, add an `Area Light`. Defaults: Disc shape,
   `radius = 0.5 m`, `Circular orbit around Z`, `speed = 18°/s`,
   `range = 60 m`. The "Live readout" section of the inspector should
   show the position orbiting (X, Y values changing every frame).
4. Verify the `safeRadius` line of the inspector — the operator's
   `orbit radius` slider may be lower than `safeRadius`; the
   *effective* radius is what's used. Try setting `orbit radius = 0`
   and confirm the light still stays outside the mesh (visual check:
   structure never goes pitch-black mid-orbit).
5. Switch shape to `Rectangle`, set `width = 4`, `height = 0.5` —
   the highlight should soften visibly (Karis α' expansion working).
6. Try `Lissajous` mode with a:b = 3:2, then `Random` with
   `dwell = 1 s` — the light should travel along curved sphere paths
   without ever crossing the mesh.

---

## 7. Performance budget

| Cost                                      | Per-fragment per area-light |
|-------------------------------------------|------------------------------|
| Range cull + facing dot                   | 1 normalize, 2 dots          |
| Diffuse closed-form form factor           | 1 div, 1 dot                 |
| Specular: representative-point projection | 1 reflect, 2 dots, 1 length  |
| Specular: GGX D, G, F                     | identical to spot/dir lights |

At `MAX_AREAS = 8` the worst-case per-fragment cost is about the same
as 4 spot lights — fine on Apple M-series at 1080p with the existing
~32 spots / 4 dirs already enabled. If a future show needs more than
8, raise `kMaxAreas` + `MAX_AREAS` together (the array layout is the
expensive part, not the loop body).

---

## 8. Future work

- Linearly-transformed-cosines (Heitz et al. 2016) for the rectangle
  path — replaces the equivalent-area disc hack with an exact
  analytic integral; needs a precomputed 2D LUT bound as a Metal
  texture.
- Orbit-gizmo overlay (draw the safe sphere + the parametric path as
  a debug line strip when the area-light layer is selected).
- Bind orbit `speedDegPerSec`, `lissA`, `lissB`, `bobFreqHz` to the
  global `ModulatorBank` (parameter-graph-spacegen skill).
