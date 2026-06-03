# INTEGRATION — Post-FX ping-pong plumbing

This document is the recipe for landing `ChromaticAberrationEffect` and
`GlitchEffect` in the live engine. The effect classes themselves are
drop-in; what needs to be added are **three small helpers on
`MetalRenderer`** and **two lines in CMakeLists.txt**.

---

## 1. Why ping-pong?

Both effects **read** the current color target as a `texture2d` and
**write** a new color. Metal explicitly forbids reading from and writing
to the same texture in a single render pass — so we need a second
texture (the "ping-pong" partner) for the output, then swap so the next
layer sees our result as `ctx.colorTarget`.

The engine already owns one color target (allocated by the windowing
code and handed to `renderFrame` via `ctx.colorTarget`). We add a
parallel "B" texture managed by `MetalRenderer`, and a flag that tracks
which one is currently "live".

```
Frame N start:
    A = live (ctx.colorTarget)
    B = scratch
StructureLayer renders into A.
ChromaticAberrationEffect:
    reads A, writes B → swaps → live = B, scratch = A
GlitchEffect:
    reads B, writes A → swaps → live = A, scratch = B
Present:
    presents A (which is what ctx.colorTarget points to after the swaps)
```

The implementation: each call to `acquirePingPongTarget(ctx)` returns
**the partner** of the texture currently pointed to by
`ctx.colorTarget`. `swapPingPong(ctx)` then mutates `ctx.colorTarget` so
it points to the just-written target. Subsequent layers see the new
target without knowing anything about the swap.

---

## 2. Required additions to `MetalRenderer.h`

Add these **public** members (alongside `renderStructureMeshes` etc.):

```cpp
// ---- Post-FX ping-pong ----
// Called by post-process effects to acquire the partner texture of
// ctx.colorTarget. Allocates / re-allocates the partner lazily so the
// dimensions and pixel format always match. Returns nullptr if alloc
// fails (effects should bail silently).
MTL::Texture* acquirePingPongTarget(const RenderContext& ctx);

// Swap the live target with the partner. After this call,
// ctx.colorTarget points to the freshly-written target.
void          swapPingPong(RenderContext& ctx);

// Shared sampler for post-FX shaders (linear filter, clamp-to-edge).
MTL::SamplerState* postFxSampler() const { return postFxSampler_; }
```

And these **private** members:

```cpp
// Ping-pong target B. The "A" side is the caller-provided ctx.colorTarget
// (typically the drawable's color attachment in the GUI build, or an
// offscreen texture in the headless build).
MTL::Texture*       pingPongB_       = nullptr;
int                 pingPongW_       = 0;
int                 pingPongH_       = 0;
MTL::PixelFormat    pingPongFmt_     = (MTL::PixelFormat)0;

// Shared sampler for post-FX (linear, clamp). The structure pipeline's
// `linearSampler_` uses repeat addressing which would wrap UVs around
// the screen — we want clamp behaviour for screen-space effects so
// channel-split offsets near the edge produce edge-bleed, not wrap.
MTL::SamplerState*  postFxSampler_   = nullptr;
```

---

## 3. Required additions to `MetalRenderer.cpp`

### In the constructor (after `buildDefaultTexturesAndSampler();`):

```cpp
// Post-FX sampler: linear filtering, clamp-to-edge.
{
    MTL::SamplerDescriptor* sd = MTL::SamplerDescriptor::alloc()->init();
    sd->setMinFilter(MTL::SamplerMinMagFilterLinear);
    sd->setMagFilter(MTL::SamplerMinMagFilterLinear);
    sd->setMipFilter(MTL::SamplerMipFilterNotMipmapped);
    sd->setSAddressMode(MTL::SamplerAddressModeClampToEdge);
    sd->setTAddressMode(MTL::SamplerAddressModeClampToEdge);
    postFxSampler_ = device_->newSamplerState(sd);
    sd->release();
}
```

### In the destructor:

```cpp
if (pingPongB_)    pingPongB_->release();
if (postFxSampler_) postFxSampler_->release();
```

### New methods:

```cpp
MTL::Texture* MetalRenderer::acquirePingPongTarget(const RenderContext& ctx) {
    if (!ctx.colorTarget) return nullptr;
    const int w = static_cast<int>(ctx.colorTarget->width());
    const int h = static_cast<int>(ctx.colorTarget->height());
    const MTL::PixelFormat fmt = ctx.colorTarget->pixelFormat();

    // Reallocate B if any property doesn't match.
    if (!pingPongB_
        || pingPongW_ != w
        || pingPongH_ != h
        || pingPongFmt_ != fmt) {
        if (pingPongB_) { pingPongB_->release(); pingPongB_ = nullptr; }
        MTL::TextureDescriptor* td = MTL::TextureDescriptor::texture2DDescriptor(
            fmt, w, h, false);
        td->setStorageMode(MTL::StorageModePrivate);
        td->setUsage(MTL::TextureUsageRenderTarget
                     | MTL::TextureUsageShaderRead);
        pingPongB_ = device_->newTexture(td);
        if (!pingPongB_) return nullptr;
        pingPongW_   = w;
        pingPongH_   = h;
        pingPongFmt_ = fmt;
    }
    return pingPongB_;
}

void MetalRenderer::swapPingPong(RenderContext& ctx) {
    if (!ctx.colorTarget || !pingPongB_) return;
    // The just-written target (pingPongB_) becomes the new "live" target.
    // The previously-live target becomes the new partner.
    MTL::Texture* prevLive = ctx.colorTarget;
    ctx.colorTarget        = pingPongB_;
    pingPongB_             = prevLive;
}
```

Note: after an **odd number** of post-FX layers in a frame, the
"original" target the host passed in no longer points to the final
image. The host (windowing code / present logic) must look at
`ctx.colorTarget` after `bus.render(ctx)` returns, not at the texture it
originally passed in. **This is the only behaviour change for the host**.

If the existing host code captures the original pointer for present,
either:
- (a) read `ctx.colorTarget` after render to discover the final target, or
- (b) add a `MetalRenderer::finalColorTarget(const RenderContext&) const`
  accessor that just returns `ctx.colorTarget` (cleaner from the host's
  POV).

---

## 4. Draw order in the bus

Effect layers run **after** all generators. The recommended ordering:

```
Bus (bottom → top):
    [Generator] StructureLayer       — meshes + lighting
    [Generator] BeamLayer × N        — packed into structure pass
    [Generator] DirectionalLightLayer × N
    [Generator] AmbientLightLayer × N
    --- post-FX zone ---
    [Effect]    BloomEffect          — when it lands
    [Effect]    ChromaticAberrationEffect
    [Effect]    GlitchEffect
    [Effect]    MotionBlurEffect     — when it lands
```

Order matters: chromatic aberration **before** glitch makes the
RGB-swapped blocks already have a CA fringe, which is the canonical
"corrupt VHS" stack. Glitch **before** CA makes the CA "clean" but the
displacement bands look very digital. Operator can re-order in the
layer rack.

The `Bus::render` loop already walks layers in order and skips
disabled / zero-opacity ones, so no scheduler changes are needed.

---

## 5. CMakeLists.txt edits

Add two source files to the `spacegen` executable target. The block to
edit is around line 247 of `engine/CMakeLists.txt`:

```cmake
add_executable(spacegen
    # ... existing sources ...
    core/Scene.cpp
    core/Layer.cpp
    core/StructureLayer.cpp
    core/BeamLayer.cpp
    core/DirectionalLightLayer.cpp
    core/AmbientLightLayer.cpp
    # ↓ add these two ↓
    next_tasks/effects/post_fx/ChromaticAberrationEffect.cpp
    next_tasks/effects/post_fx/GlitchEffect.cpp
    # ... rest unchanged ...
)
```

The `.metal.inc` files are `#include`d as raw-string literals from the
`.cpp` files, so they don't need to be listed in CMake. Make sure
include paths reach `engine/next_tasks/effects/post_fx/` — relative
`#include "ca.metal.inc"` works because the .cpp is compiled with its
own directory on the include path by default (CMake's source-file
handling); if not, add:

```cmake
target_include_directories(spacegen PRIVATE
    next_tasks/effects/post_fx
)
```

The headers are included via relative paths from the .cpp files
(`../../../backends/metal/MetalRenderer.h` etc.), so no further include
config is needed.

---

## 6. Adding to the scene from code

```cpp
// e.g. in main.cpp after scene construction:
scene.bus.add<spacegen::ChromaticAberrationEffect>();
scene.bus.add<spacegen::GlitchEffect>();
```

Or via the layer-rack "+ Add Layer" menu. The menu source is somewhere
like `engine/gui/LayerRackPanel.cpp` — add an entry there to expose
"Add → Effect → Chromatic Aberration" / "Add → Effect → Glitch".

---

## 7. Test sanity

After integration:

1. Build (`cmake --build build -j`). MSL compile happens at runtime so
   any shader error will show up only when the effect first runs.
2. Add a `ChromaticAberrationEffect` to the bus, set strength to 0.05.
   You should see edges of the structure get a red/blue fringe.
3. Add a `GlitchEffect`, set master amount to 0.5, BPM to 128. On every
   beat you should see displacement bands + RGB swap snap into view.
4. Disable both effects. The frame should be pixel-identical to before
   (the bypass paths take the `strength <= 1e-5` and `A <= 1e-4` early
   returns).
5. Strength / amount = 0 should still incur the pass (the pipeline runs
   and the bypass branch passes through). To skip the pass entirely,
   the layer's `LayerState` must be Disabled or `opacity` must be 0 —
   handled by `Bus::render`.

---

## 8. Performance notes

- CA: ~3 texture samples per fragment. At 1080p, < 0.2ms on M1 — the
  pass is bandwidth-limited, not ALU.
- Glitch: 1 base sample + ~5 hash calls + branchy sub-effect logic.
  At 1080p, ~0.4ms on M1. The threshold branches mean cost scales with
  amount: ~0.1ms at amount=0, ~0.4ms at amount=1.0.
- Ping-pong target is `PixelFormatPrivate` (GPU-only) — no PCIe traffic.
- A real bottleneck would be running 5+ post-FX in a chain on a low-end
  GPU. The flat ping-pong scheme means each effect doubles the per-frame
  fillrate cost vs. having no post-FX, but stays linear in effect count.
