# Integrating SDFParticleLayer into SpaceGen

This is a self-contained Layer — most plumbing already exists in `RenderContext`
(see `core/Layer.h`). Only three changes need to land in `MetalRenderer`:

1. Expose `MTL::Device*` so the layer can allocate its own buffers + compile
   its own MSL library.
2. Expose the depth texture so the particle render pass can depth-test
   against the structure pass output.
3. Wire the new source files into `CMakeLists.txt`.

Plus one build-side detail: the MSL is read at runtime from a C++ raw-string
literal — we wrap `particles.metal.inc` into a `.literal` companion file at
configure time. See §3.

## 1. MetalRenderer API additions

Add two trivial public getters to `engine/backends/metal/MetalRenderer.h`,
right next to the existing `projection()` / `view()` / `cameraWorldPos()`
accessors:

```cpp
// engine/backends/metal/MetalRenderer.h

// For FX layers that need to allocate their own GPU resources or compile
// their own shader libraries (particles, fluids, raymarchers, etc.).
MTL::Device*   devicePublic()        const { return device_; }

// For FX layers that need to depth-test against the structure pass output.
// Read-only — they do NOT write depth themselves.
MTL::Texture*  depthTexturePublic()  const { return depthTex_; }
```

Both are non-owning, single-line inline getters. No state changes. No new
dependencies. `device_` is the existing `MTL::Device*` field; `depthTex_`
is the existing offscreen depth texture (32-bit float, sized to the color
target via `onResize`).

Confirm `MetalRenderer::renderFrame(...)` calls `onResize` before walking
the bus — it already does (see line 762 of `MetalRenderer.cpp`):

```cpp
void MetalRenderer::renderFrame(RenderContext& ctx, Bus& bus) {
    if (!ctx.cmdBuf || !ctx.colorTarget) return;
    onResize(static_cast<int>(ctx.colorTarget->width()),
             static_cast<int>(ctx.colorTarget->height()));
    bus.render(ctx);
}
```

So `depthTex_` is guaranteed sized correctly when SDFParticleLayer::render
runs.

## 2. Add the layer to the bus

In your scene-construction code (typically `main.cpp` or a Workstation
initializer), add an `SDFParticleLayer` after the `StructureLayer` so it
renders on top of the structure (additive blend = it just adds light to
whatever the structure already wrote):

```cpp
#include "next_tasks/effects/sdf_particles/SDFParticleLayer.h"

// ... after building structure + lights ...
scene.bus.add<spacegen::SDFParticleLayer>();
```

The layer auto-derives the bbox + Surface emission CDF from `ctx.scene`
on first render. No explicit init step needed.

## 3. Wrap the MSL into a raw-string literal

`SDFParticleLayer.cpp` does:

```cpp
constexpr const char* kSDFParticlesMSL =
#include "particles.metal.inc.literal"
;
```

`particles.metal.inc.literal` is **generated** from `particles.metal.inc`
at configure time by wrapping the latter in `R"MSL(...)MSL"`. We do it
this way so that `particles.metal.inc` stays a plain, lintable, syntax-
highlighted MSL file for the human reading it.

Two equivalent options:

### Option A — CMake `configure_file` with a small wrapper file

Add to `engine/CMakeLists.txt`, right after the `add_executable(spacegen ...)`
block:

```cmake
# Wrap particles.metal.inc into a C++ raw-string literal.
set(_SDF_PARTICLES_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/next_tasks/effects/sdf_particles")

file(READ "${_SDF_PARTICLES_DIR}/particles.metal.inc" _SDF_PARTICLES_MSL)
file(WRITE
    "${CMAKE_CURRENT_BINARY_DIR}/sdf_particles_generated/particles.metal.inc.literal"
    "R\"MSL(\n${_SDF_PARTICLES_MSL}\n)MSL\"\n")

target_include_directories(spacegen PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/sdf_particles_generated")
```

(This regenerates the literal at every CMake configure step. For
auto-regeneration on `.metal.inc` edits, wrap it with `add_custom_command`
and a `DEPENDS`.)

### Option B — Plain shell command, run once

Equivalent one-liner you can run manually:

```sh
{ printf 'R"MSL(\n'; cat particles.metal.inc; printf ')MSL"\n'; } \
  > particles.metal.inc.literal
```

Either way, the produced file must sit at a location reachable from the
include path used by the `.cpp`. The CMake snippet above puts it under
`build/sdf_particles_generated/`.

## 4. CMakeLists.txt — source list

Add the new sources to the `add_executable(spacegen ...)` block in
`engine/CMakeLists.txt`, alongside the other layer sources:

```cmake
add_executable(spacegen
    main.cpp
    metal_cpp_impl.cpp
    tinygltf_impl.cpp
    platform_metal_glfw.mm
    core/Scene.cpp
    core/Layer.cpp
    core/StructureLayer.cpp
    core/BeamLayer.cpp
    core/DirectionalLightLayer.cpp
    core/AmbientLightLayer.cpp
    core/SyphonInputLayer.mm
    core/MeshDecimate.cpp
    core/UvAtlas.cpp
    core/UvAtlasGeogram.cpp
    core/UvOptimize.cpp
    backends/metal/MetalRenderer.cpp
    gui/Workstation.mm
    # ---- next_tasks ---------------------------------------------------
    next_tasks/effects/sdf_particles/SDFParticleLayer.cpp
    # -------------------------------------------------------------------
    ${IMGUI_SOURCES}
)
```

The header `SDFParticleLayer.h` and the MSL source `particles.metal.inc`
do not need to be in the source list (they're `#include`'d transitively).

If you want them visible in Xcode/IDE indexers, add a separate
`add_custom_target` or include them in `target_sources(... PRIVATE ...)`
under a `INTERFACE` flag — purely cosmetic.

## 5. RenderContext touch-ups (none required)

The current `RenderContext` already carries everything we need:

| Field             | Used for                                       |
|-------------------|------------------------------------------------|
| `renderer`        | `devicePublic()`, `depthTexturePublic()`       |
| `scene`           | mesh CDF, modulator bank, bbox                 |
| `cmdBuf`          | `computeCommandEncoder`, `renderCommandEncoder`|
| `colorTarget`     | accumulator target (Load + additive blend)     |
| `width / height`  | aspect, viewport packing                       |
| `projection/view` | uniforms                                       |
| `cameraWorldPos`  | uniforms (for distance-based heuristics later) |
| `elapsedSeconds`  | curl-noise time, dt computation                |
| `frameIndex`      | PRNG seed perturbation                         |

No changes needed.

## 6. Buffer ownership / lifetime

- `particleBuf_`, `trailRingBuf_`: GPU-private, owned by the layer.
  Realloc on count/trail-length change.
- `triCDFBuf_`, `triVertsBuf_`: shared (CPU-visible) storage so we can
  upload from CPU once at first render. Owned by the layer.
- `trailHeadBuf_`: GPU-private, per-particle uint32 head index. Sized to
  match `particleBuf_` capacity; allocated whenever `trailLength > 1`.
- All Metal objects are released in `releaseAllGpuResources()` from the
  dtor.

## 7. Testing checklist

After integrating:

1. Build. The MSL must compile at runtime; any error logs to stderr
   prefixed `[SDFParticleLayer]`.
2. Add the layer to your scene; with default params you should see a
   warm-to-cool haze drifting around the structure.
3. Drag `SDF strength` to +5 → particles collapse into a thin halo
   hugging the box.
4. Drag `SDF strength` to -5 → particles flee outward, leaving a void
   around the structure.
5. Drag `Curl amplitude` to 10 → motion becomes turbulent / chaotic.
6. Set `Trail length` to 16 → output becomes ribbon-like; count auto-
   caps at 256k.
7. Switch `Emitter` to `Volume` → particles initialize uniformly inside
   the bbox.
8. Switch to `Point` → all particles spawn at `pointOrigin`, fan outward.
9. Sanity perf: 100k particles, point sprites, ~1 ms compute + ~0.5 ms
   render on M1 Max @ 1080p.

## 8. Performance & memory notes

- Buffer storage: `MTLResourceStorageModePrivate` for `particleBuf_` and
  `trailRingBuf_` — no CPU access needed after the first GPU init pass
  (which itself is implicit: zero-filled memory has `lifetime == 0` so
  the first update-kernel call triggers respawn for every particle).
- The Surface emitter does a serial binary search on the CDF in the
  shader. For 100k tris this is ~17 iterations per respawn. Acceptable
  on M1 (~5 % of update-kernel time). If you push to a 1M-tri structure,
  consider a Walker's alias table instead (constant-time draws).
- Sprite rasterisation is overdraw-bound at large `pointSizePx`. Keep
  the size under ~8 px for 1 M particles or you'll fill-rate yourself
  off-budget at 4K.

## 9. Known limitations / v2 work

- **No GPU sort**: additive blend is order-independent; if we ever add a
  non-additive blend mode, we'll need a depth sort (MPS bitonic).
- **Box-only SDF**: §3.b of README outlines the baked 3D-texture upgrade.
- **No spawn burst on transient**: audio-reactive burst spawning is on
  the roadmap; the layer already exposes `*ModSlot` / `*ModDepth` pairs
  for the continuous bindings, but transient ("hit on kick") is a
  separate API extension.

## 10. Files added

```
engine/next_tasks/effects/sdf_particles/
  ├── README.md                    (algorithm spec, math, references)
  ├── SDFParticleLayer.h           (class declaration)
  ├── SDFParticleLayer.cpp         (~430 LOC implementation)
  ├── particles.metal.inc          (~370 LOC MSL — compute + render)
  └── INTEGRATION.md               (this file)
```

Total: ~880 LOC code + ~520 LOC doc.
