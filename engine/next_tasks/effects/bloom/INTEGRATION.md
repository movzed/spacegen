# BloomEffect — Integration notes

This document lists everything that has to happen outside of
`/engine/next_tasks/effects/bloom/` for `BloomEffect` to be wired into
the workstation. It is intentionally minimal — `BloomEffect` was designed
to be self-contained (it owns its Metal library, pipelines, sampler, and
mip-chain textures), so the integration footprint is small.

The deliverable in this folder is drop-in: `BloomEffect.h`,
`BloomEffect.cpp`, and `bloom.metal.inc`. Move the .h + .cpp wherever the
project organises layer source (matching `BeamLayer`, `StructureLayer`,
etc.) — the canonical location in v1 would be `engine/core/`.

---

## 1. CMakeLists.txt — add the source file

If the bloom files live in `engine/next_tasks/effects/bloom/`, the
canonical pattern (matching the existing `core/` layers) is to glob them
in alongside the rest:

```cmake
# In engine/CMakeLists.txt, wherever the SOURCES list is assembled:
list(APPEND SOURCES
    next_tasks/effects/bloom/BloomEffect.cpp
)
```

No new headers need to be added — `BloomEffect.h` is included only from
the workstation (or wherever the new layer is constructed). If the team
prefers them in `engine/core/effects/`, just move both files there and
update the path.

`bloom.metal.inc` does **not** need to be added to `SOURCES`. It is not
compiled as a Metal artifact — the shader source is embedded in
`BloomEffect.cpp` via the `kBloomMSLFull` constant. The .inc file is
kept on disk purely for human review (and so the algorithm is editable
in a separate file with proper Metal-syntax highlighting).

No other CMake changes are required. **In particular, do not add any
extra link libraries** — bloom uses only Metal-cpp and ImGui, both of
which the engine already links against.

---

## 2. Construction

Add bloom from the Layers panel toolbar — same pattern as the other "+"
buttons in `Workstation.mm`'s `drawLayerRack`:

```cpp
// In drawLayerRack() in Workstation.mm, alongside "+ Spot" / "+ Syphon":
if (ImGui::Button("+ Bloom")) {
    auto* b = scene.bus.add<spacegen::BloomEffect>();
    char buf[64];
    std::snprintf(buf, sizeof(buf), "Bloom %zu", scene.bus.layers.size());
    b->name = buf;
    selectedLayerId = b->id;
}
```

…and the include:

```cpp
#include "../next_tasks/effects/bloom/BloomEffect.h"
// or "../core/BloomEffect.h" if you moved the file
```

That is the only workstation change. No menu items, no panels, no event
plumbing — the inspector is `drawInspector()` on the layer itself, picked
up automatically by `drawInspector(scene, selectedLayerId_, …)`.

---

## 3. Layer ordering

Effects in SpaceGen render in bus order, like any other layer. For bloom
to work as a global post-process, place it **after** every Generator
that draws into `ctx.colorTarget`. In v1 that means:

```
Master bus
├─ Structure                  (Generator — writes colorTarget)
├─ Spot lights, Directional, Ambient, Syphon, …
├─ … other Generators …
└─ Bloom (Karis/Jimenez)      (Effect — reads + writes colorTarget)
```

When the operator adds Bloom via the button above, it lands at the end
of the list (which is exactly where it should sit). If they reorder it,
the engine still behaves correctly — but bloom placed before any
Generator simply has nothing to bloom over.

In a future version where `Bus` exposes ordering controls, the rack UI
can grey out "move bloom up" past the last Generator to make this
guard rail explicit.

---

## 4. MetalRenderer — no changes required

`BloomEffect::render(ctx)` uses only what is already exposed in
`RenderContext`:

```cpp
struct RenderContext {
    MTL::CommandBuffer*  cmdBuf;        // ← we encode into this
    MTL::Texture*        colorTarget;   // ← we read AND blit-back here
    int                  width, height;
    // (unused by bloom: scene, renderer, projection, view, …)
};
```

The colorTarget already has the right usage flags. From `Workstation.mm`:

```cpp
td->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
```

`ShaderRead` lets us sample it as input in the threshold and composite
passes; `RenderTarget` lets us blit the composite scratch over it as
the final step. No changes to those flags are needed.

### Optional fast-path (do not block on this)

If the project later wants to avoid the final blit, two paths exist:

1. **Add `MTL::TextureUsagePixelFormatView`** to the colorTarget so the
   composite can write to a `texture2d_array`-view of the same memory
   that lives in a different command-encoder pass. Cost: one extra
   flag in `Workstation::ensureOffscreen`. Savings: ~5% on the bloom
   pass on Apple Silicon (the blit is already cheap).
2. **Promote bloom to a tile-resident programmable blending pass** on
   Apple GPUs (where tile memory persists across passes). This is
   genuine work — currently out of scope.

Neither is recommended for the v1 ship. The blit is sub-millisecond.

---

## 5. Pixel format note

`BloomEffect` queries `ctx.colorTarget->pixelFormat()` and rebuilds its
pipeline states if the format changes. In the current workstation the
format is `BGRA8Unorm` (matching the swapchain), which is fine —
intensity > 1 simply clamps when written. If the project later switches
the offscreen to `RGBA16Float` for HDR bloom (recommended), no code in
`BloomEffect` needs to change — the rebuild path handles it
automatically on the first frame after the switch.

For a true HDR look, the threshold should be raised proportionally to
the new dynamic range — e.g. set `threshold = 1.5` and let intensity
take over. The soft-knee makes this transition forgiving.

---

## 6. Test plan

1. **Smoke**: add a Bloom layer with default values to a scene that
   already contains one or more Spot Lights. The visible cones / hot
   pixels should grow soft haloes. With `intensity = 0` the image is
   unchanged.
2. **Threshold sweep**: ramp `threshold` from 0 to 2 — at 0 the entire
   image blooms (overcooked); at 2 only very bright spots do (subtle).
3. **Radius sweep**: ramp `radius` from 4 to 7 — the halo should
   visibly widen.
4. **Tint**: set `tint = (1.0, 0.85, 0.55)`; the bloom should turn
   gold. Set `(0.6, 0.8, 1.0)`; icy.
5. **Soft-knee**: with the scene mostly dark, push `threshold = 1.0`
   and observe the "pop" of a moving bright pixel crossing the
   boundary. With `softKnee = 0` it pops; with `softKnee = 0.5` it
   fades in.
6. **Order independence**: drag a Bloom layer above the Structure
   layer in the rack — the image should NOT bloom (nothing has
   written colorTarget yet at the point bloom runs). This is the
   expected guard-rail behaviour.
7. **Resize**: resize the GLFW window so the workstation reallocates
   the offscreen. Bloom should keep working with no visible glitch
   (the mip chain rebuilds on the next frame).
8. **Multiple bloom layers**: stack two Bloom effects with different
   intensities. They should compose correctly (each one reads the
   colorTarget the previous one wrote).

---

## 7. References (for code reviewers)

- Karis, B. (2013). *Real Shading in Unreal Engine 4.* SIGGRAPH 2013
  Physically Based Shading in Theory and Practice course notes.
- Jimenez, J. (2014). *Next Generation Post Processing in Call of Duty:
  Advanced Warfare.* SIGGRAPH 2014 Advances in Real-Time Rendering
  course notes — slides 153 (downsample) and 162 (upsample).
- See `README.md` next to this file for full kernels and citations.
