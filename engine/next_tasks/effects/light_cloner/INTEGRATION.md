# LightClonerLayer — Integration Guide

Wiring `LightClonerLayer` into SpaceGen requires touching three files:

1. `engine/backends/metal/MetalRenderer.cpp` — raise `kMaxSpots` and (when
   `AreaLightLayer` lands) add `MAX_AREA` arrays.
2. `engine/core/StructureLayer.cpp` — collect `LightClonerLayer` instances
   from the bus and call `expandSpots()` to append virtual fixtures to the
   spot-light array before the structure pass uploads its uniforms.
3. `engine/CMakeLists.txt` — register the new sources.

This guide gives the EXACT diff sites and the replacement snippets.

---

## 1. `engine/backends/metal/MetalRenderer.cpp`

### 1a. Raise `kMaxSpots` (and the MSL mirror)

The cloner can emit up to 64 virtual spots in a single layer; combined with
the existing `BeamLayer` rigs (up to 8 per layer), the renderer ceiling
needs to be at least 64. We bump it cleanly.

Find at ~line 25:

```cpp
constexpr int kMaxSpots = 32;   // up to ~4 rigs × 8 fixtures
constexpr int kMaxDirs  = 4;
```

Replace with:

```cpp
constexpr int kMaxSpots = 64;   // BeamLayer rigs + LightClonerLayer (cloner ≤ 64)
constexpr int kMaxDirs  = 4;
```

In the embedded MSL source (~line 32) find:

```cpp
constant int   MAX_SPOTS  = 32;
constant int   MAX_DIRS   = 4;
```

Replace with:

```cpp
constant int   MAX_SPOTS  = 64;
constant int   MAX_DIRS   = 4;
```

That's the only MSL-side change needed — the shader's spot-loop reads
`metallicCounts.z` for the actual count each frame, so we never iterate
empty entries.

### 1b. (Optional, when `AreaLightLayer` lands) add MAX_AREA arrays

This section only applies AFTER `AreaLightLayer` ships. Until then, the
cloner's `LightKind::Area` branch is a no-op and you can skip this section.

When `AreaLightLayer` lands, add to the MSL header (next to `MAX_SPOTS`):

```cpp
constant int   MAX_AREA   = 16;
```

Add the matching CPU-side constexpr:

```cpp
constexpr int kMaxArea = 16;
```

Add a CPU mirror struct (next to `GpuSpot`):

```cpp
struct GpuArea {
    glm::vec4 posIntensity;   // .xyz pos, .w intensity
    glm::vec4 normalSize;     // .xyz normal (FROM light), .w halfSize
    glm::vec4 colorWidth;     // .rgb color, .a width (for rectangular area)
    glm::vec4 paramsHeight;   // .x height, .y twoSided (0/1), .z falloffExp
};
```

…and an MSL `AreaLight` struct, plus `AreaLight areas[MAX_AREA]` in
`Uniforms`, plus a `.x` count slot in some new uniform vec4. Forward a
`std::vector<const AreaLightLayer*>& areas` parameter to
`renderStructureMeshes()` and pack it like the spots block. Symmetric work.

The cloner is already ready: `expandAreas()` would mirror `expandSpots()`
(returning `VirtualArea` rather than `VirtualSpot`) and live in a follow-up
commit alongside `AreaLightLayer`.

### 1c. The spot packing loop already handles cloners transparently

In `renderStructureMeshes()` at ~line 820, the loop:

```cpp
GpuSpot spotsPacked[kMaxSpots]{};
int spotCount = 0;
for (const BeamLayer* s : spots) {
    if (!s || spotCount >= kMaxSpots) break;
    ...
}
```

is fed by `StructureLayer.cpp`. After the integration in §2, the `spots`
vector is unchanged (still `vector<const BeamLayer*>`), but a SECOND vector
of pre-baked `GpuSpot`-equivalent entries comes pre-packed from the cloners
and is concatenated before the loop runs. See §2 for the exact code.

No further changes to MetalRenderer are needed.

---

## 2. `engine/core/StructureLayer.cpp`

We extend the bus walk to also collect `LightClonerLayer` instances, then
expand each one into a `std::vector<VirtualSpot>` that is forwarded to the
renderer alongside the existing `BeamLayer*` list.

### 2a. Add the include

At the top, next to the other layer includes:

```cpp
#include "../next_tasks/effects/light_cloner/LightClonerLayer.h"
```

(If/when this file is promoted out of `next_tasks/` into `core/`, the
include path becomes `"LightClonerLayer.h"`.)

### 2b. Extend the bus walk

In `StructureLayer::render()` find:

```cpp
std::vector<const BeamLayer*>             spots;
std::vector<const DirectionalLightLayer*> dirs;
glm::vec3 ambientColor(0.0f);
```

Add a virtual-spot list alongside:

```cpp
std::vector<const BeamLayer*>             spots;
std::vector<const DirectionalLightLayer*> dirs;
std::vector<VirtualSpot>                   virtualSpots;
glm::vec3 ambientColor(0.0f);
```

In the same function find the dispatch block:

```cpp
if (auto* b = dynamic_cast<const BeamLayer*>(l.get())) {
    spots.push_back(b);
} else if (auto* d = dynamic_cast<const DirectionalLightLayer*>(l.get())) {
    dirs.push_back(d);
} else if (auto* a = dynamic_cast<const AmbientLightLayer*>(l.get())) {
    ambientColor += a->color * (a->intensity * a->opacity);
}
```

Add a `LightClonerLayer` branch BEFORE the Syphon branch (order in the
chain doesn't matter except that more specific dynamic_casts come first):

```cpp
if (auto* b = dynamic_cast<const BeamLayer*>(l.get())) {
    spots.push_back(b);
} else if (auto* c = dynamic_cast<const LightClonerLayer*>(l.get())) {
    c->expandSpots(ctx, virtualSpots);     // appends N entries
} else if (auto* d = dynamic_cast<const DirectionalLightLayer*>(l.get())) {
    dirs.push_back(d);
} else if (auto* a = dynamic_cast<const AmbientLightLayer*>(l.get())) {
    ambientColor += a->color * (a->intensity * a->opacity);
} else if (auto* s = dynamic_cast<SyphonInputLayer*>(l.get())) {
    // ... unchanged
}
```

### 2c. Forward `virtualSpots` to the renderer

`renderStructureMeshes()` currently takes `std::vector<const BeamLayer*>&`.
Add an overload (or a second parameter) for the pre-baked virtual spots.
The cleanest way is to extend the existing signature with a new optional
parameter — but to keep the renderer header light, we instead pack the
clone fixtures into the same `GpuSpot`-equivalent format in
`StructureLayer.cpp` and forward them via a new parameter
`const std::vector<VirtualSpot>& extraSpots`.

In `MetalRenderer.h` extend `renderStructureMeshes`:

```cpp
void renderStructureMeshes(
    RenderContext& ctx,
    const StructureLayer& layer,
    const std::vector<const BeamLayer*>& spots,
    const std::vector<const DirectionalLightLayer*>& dirs,
    const glm::vec3& ambientColor,
    const std::vector<VirtualSpot>& extraSpots = {},   // NEW
    MTL::Texture* syphonTex    = nullptr,
    ...
```

Add a forward-decl `struct VirtualSpot;` at the top of `MetalRenderer.h`
(or include `LightClonerLayer.h` if you don't mind the dependency). The
simplest is a forward-decl + a `#include` in the .cpp.

In `MetalRenderer.cpp::renderStructureMeshes()` at the end of the existing
`for (const BeamLayer* s : spots)` loop, append:

```cpp
for (const VirtualSpot& v : extraSpots) {
    if (spotCount >= kMaxSpots) break;
    spotsPacked[spotCount].posIntensity =
        glm::vec4(v.worldPos, v.intensity);
    spotsPacked[spotCount].dirRange     =
        glm::vec4(v.direction, v.range);
    spotsPacked[spotCount].colorInner   =
        glm::vec4(v.color, v.innerCos);
    spotsPacked[spotCount].paramsOuter  =
        glm::vec4(v.outerCos, 0.0f, 0.0f, 0.0f);
    ++spotCount;
}
```

And forward `virtualSpots` from `StructureLayer.cpp`:

```cpp
ctx.renderer->renderStructureMeshes(ctx, *this, spots, dirs,
                                      ambientColor,
                                      virtualSpots,   // NEW
                                      syphonTex, syphonMix, syphonTint,
                                      syphonFlipY,
                                      showStretchHeatmap,
                                      stretchMetric, stretchUV,
                                      syphonProjFlatMix,
                                      syphonProjFlatThresh);
```

That's it for `StructureLayer.cpp`. Total: 4 small edits (1 include, 1
vector decl, 1 new dispatch branch, 1 extra argument to the renderer call).

---

## 3. `engine/CMakeLists.txt`

Add the two new source files to the engine library target. Find the block
where the core layer .cpp files are listed (e.g. `BeamLayer.cpp`,
`StructureLayer.cpp`) and add alongside:

```cmake
add_library(spacegen_engine STATIC
    core/Layer.cpp
    core/Scene.cpp
    core/StructureLayer.cpp
    core/BeamLayer.cpp
    core/AmbientLightLayer.cpp
    core/DirectionalLightLayer.cpp
    # ... existing entries ...
    next_tasks/effects/light_cloner/LightClonerLayer.cpp   # NEW
    # ...
)
```

And the include path so other TUs can find `LightClonerLayer.h`:

```cmake
target_include_directories(spacegen_engine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/next_tasks/effects/light_cloner   # NEW
    # ... existing ...
)
```

When the cloner graduates from `next_tasks/` to `core/`, both lines move
to the canonical `core/` path. No other CMake changes (no shaders, no
linker flags, no third-party deps).

---

## Smoke test (after wiring)

In whatever helper builds the default scene (`buildDefaultScene` or
similar), add:

```cpp
auto* cloner = scene.bus.add<LightClonerLayer>();
cloner->pattern      = LightClonerLayer::Pattern::FibSphere;
cloner->clones       = 32;
cloner->radius       = 4.0f;
cloner->colorScheme  = LightClonerLayer::ColorScheme::HueChase;
cloner->colorStart   = glm::vec3(1.0f, 0.2f, 0.4f);
cloner->hueSpread    = 1.0f;
cloner->phaseSpread  = 1.0f;
cloner->aimInward    = true;
cloner->templateIntensity = 3.0f;
cloner->templateInnerDeg  = 4.0f;
cloner->templateOuterDeg  = 7.0f;
cloner->motionLFO.pattern = MotionLFO::Pattern::Circle;
cloner->motionLFO.freqHz  = 0.2f;
cloner->motionLFO.panAmp  = 8.0f;
cloner->motionLFO.tiltAmp = 8.0f;
```

You should see a rainbow Fibonacci ball of 32 spots wrapped around the
scene centroid, each rotating in a small circle ~5° off its base aim, with
the hue chasing around the sphere. Drop `clones` to 1 to confirm the
single-light case still degenerates correctly. Drop `phaseSpread` to 0 to
confirm all 32 lights sweep in perfect unison.

## Failure modes / gotchas

- **`spotCount` overflow**: if you have multiple cloners + several
  `BeamLayer` rigs you can exceed `kMaxSpots = 64`. The renderer silently
  truncates (the `break` on `spotCount >= kMaxSpots` in the loop above).
  This is the same behavior as the existing `BeamLayer` packing — no extra
  error path needed.
- **Identical positions**: if `radius = 0` (Ring/Helix/FibSphere) or
  `randomRadius = 0`, all clones overlap on the centroid. `cloneDirection`
  handles the degenerate `clonePos == centroid` case explicitly (falls
  back to world +Y).
- **`useSceneCentroid` with empty scene**: `effectiveOrigin` falls back to
  the configured `origin` field if `ctx.scene->centroid` is not finite.
- **Order of bus traversal**: the existing bus walk is bottom-to-top by
  the order layers were added. Cloners are collected in that same order
  and appended after the manually-added BeamLayer fixtures. Within the
  light arrays of a frame, this only matters for the `MAX_SPOTS`
  truncation behavior — first-added wins.
