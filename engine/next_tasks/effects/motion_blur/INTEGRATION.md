# Integration: MotionBlurEffect

This document is the operator's checklist for landing MotionBlurEffect into
the SpaceGen tree. It is organized in **two phases**:

* **Phase A** — camera-only path. Drop-in. No renderer / shader changes
  outside the new files. Recommended as the first commit; ships motion blur
  to the operator immediately.
* **Phase B** — per-object velocity buffer (MRT during structure pass).
  Optional upgrade. Adds a second render target and modifies the structure
  shader's vertex+fragment stages.

The two phases can ship as separate PRs. Phase B is strictly additive — it
adds capability without changing Phase A behavior.

---

## Phase A — drop-in camera-only

### Files added (build system only — *do not* edit CMakeLists.txt; let the
operator wire it in):

```
engine/next_tasks/effects/motion_blur/MotionBlurEffect.h
engine/next_tasks/effects/motion_blur/MotionBlurEffect.cpp
engine/next_tasks/effects/motion_blur/motion_blur.metal.inc
```

The `.cpp` `#include`s `motion_blur.metal.inc` directly via a raw-string
trick — keep both files together. When promoted out of `next_tasks/`, the
final landing spot is `engine/core/` (alongside the other layers).

### MetalRenderer changes

**None for Phase A.** The effect reads from `ctx.colorTarget`, owns its own
scratch ping-pong texture, and writes back into the same color attachment.
Nothing in `MetalRenderer.{h,cpp}` needs to change.

### Workstation changes

Add a "+ Motion Blur" button to the layer rack in `Workstation.mm`,
mirroring the existing pattern around line 197 (`+ Ambient`, `+ Syphon`).
The diff is:

```cpp
// Workstation.mm — drawLayerRack(), after the "+ Syphon" button:
ImGui::SameLine();
if (ImGui::Button("+ Motion Blur")) {
    auto* m = scene.bus.add<spacegen::MotionBlurEffect>();
    char buf[64];
    std::snprintf(buf, sizeof(buf), "Motion Blur %zu",
                   scene.bus.layers.size());
    m->name = buf;
    selectedLayerId = m->id;
}
```

…plus the `#include`:

```cpp
#include "../next_tasks/effects/motion_blur/MotionBlurEffect.h"
```

(or `#include "../core/MotionBlurEffect.h"` once the file moves there).

### Layer order recommendation

MotionBlurEffect is a screen-space effect and should sit **above** all
generators that contribute to the color target the operator wants to smear,
and **below** any UI / debug overlays. With v1's bus, that means: add it
last in the rack so it runs after StructureLayer, BeamLayer, SyphonInput,
etc. The Bus renders bottom-to-top.

### Verification (Phase A)

1. Open SpaceGen with the standard sample scene.
2. Layer rack → `+ Motion Blur`. Confirm the inspector shows the sliders.
3. With the camera fully static (no nudges to `scene.camera`), confirm the
   image is **bit-identical** to no-blur — velocity is zero. (If it isn't,
   the prev-matrix shift is mistimed; check `render()`'s final two lines.)
4. Animate the camera in code (e.g. add a 1-degree-per-frame yaw to
   `scene.camera.view` temporarily). The structure should smear along the
   rotation direction. Increasing `shutter` should exaggerate; decreasing
   `sampleCount` should coarsen.
5. Disable and re-enable the layer mid-frame — the first re-enable frame
   should **not** show a wild smear (the prev cache is invalidated; see
   `lastStateSeen_` logic in `render()`).
6. Resize the window. The scratch texture must be re-created at the new
   size; check the inspector's "Scratch tex" readout matches the new
   composition dock dimensions.

---

## Phase B — per-object velocity buffer

This phase adds a `RG16F` velocity attachment to the structure pass.
**Mandatory shader edits** to the PBR vertex+fragment stages. Recommend
shipping under a renderer build-time flag (`SPACEGEN_VELOCITY_MRT=ON`) so
the default build stays Phase-A only.

### MetalRenderer.h

Add the following private state to `class MetalRenderer`:

```cpp
// ---- Velocity MRT (motion blur Path B) ----
// Owns a RG16F render target the same size as the depth texture.
// Reallocated alongside it.
MTL::Texture*    velocityTex_  = nullptr;
int              velocityW_    = 0;
int              velocityH_    = 0;

// Previous-frame matrices, shifted at end-of-renderFrame. Read by the
// structure vertex shader to compute prevClipPos.
glm::mat4        prevProjection_ = glm::mat4(1.0f);
glm::mat4        prevView_       = glm::mat4(1.0f);
bool             prevMatricesValid_ = false;
```

Public accessor (consumed by `MotionBlurEffect::render`):

```cpp
public:
    MTL::Texture* velocityTexture() const { return velocityTex_; }
    const glm::mat4& prevProjection() const { return prevProjection_; }
    const glm::mat4& prevView()       const { return prevView_;       }
```

### MetalRenderer.cpp — pipeline build

In `buildPipeline()`, before `device_->newRenderPipelineState`, add the
second color attachment:

```cpp
pd->colorAttachments()->object(1)->setPixelFormat(MTL::PixelFormatRG16Float);
pd->colorAttachments()->object(1)->setBlendingEnabled(false);
```

### MetalRenderer.cpp — texture allocation

In `onResize()`, alongside the depth allocation, allocate `velocityTex_`:

```cpp
if (velocityTex_) { velocityTex_->release(); velocityTex_ = nullptr; }
MTL::TextureDescriptor* vd = MTL::TextureDescriptor::texture2DDescriptor(
    MTL::PixelFormatRG16Float, widthPixels, heightPixels, false);
vd->setStorageMode(MTL::StorageModePrivate);
vd->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
velocityTex_ = device_->newTexture(vd);
velocityW_   = widthPixels;
velocityH_   = heightPixels;
```

### MetalRenderer.cpp — structure pass

In `renderStructureMeshes()`, after binding the color attachment, also bind
the velocity attachment and set its clear/load actions:

```cpp
auto* velAttachment = rpd->colorAttachments()->object(1);
velAttachment->setTexture(velocityTex_);
velAttachment->setLoadAction(MTL::LoadActionClear);
velAttachment->setStoreAction(MTL::StoreActionStore);
velAttachment->setClearColor(MTL::ClearColor(0.0, 0.0, 0.0, 0.0));
```

Pass `prevProjection_`, `prevView_`, and per-mesh `prevTransform` into the
Uniforms struct (extend the struct + MSL declaration). Add the previous-
frame uniforms:

```glsl
float4x4 prevProjection;
float4x4 prevView;
float4x4 prevModel;
```

### MetalRenderer.cpp — end of renderFrame

After `bus.render(ctx)`, shift the matrices:

```cpp
void MetalRenderer::renderFrame(RenderContext& ctx, Bus& bus) {
    // ... existing onResize + bus.render ...
    prevProjection_   = projection_;
    prevView_         = view_;
    prevMatricesValid_ = true;
}
```

And in `loadScene()` (where you set `projection_`, `view_`), also seed the
previous matrices on the first scene load:

```cpp
if (!prevMatricesValid_) {
    prevProjection_   = projection_;
    prevView_         = view_;
    prevMatricesValid_ = true;
}
```

### Structure shader (MSL) — vertex stage

Extend `VertexOut` with two interpolants and write them in `vs_main`:

```glsl
struct VertexOut {
    // ... existing fields ...
    float4 currClipPos;
    float4 prevClipPos;
};

vertex VertexOut vs_main(VertexIn in [[stage_in]],
                         constant Uniforms& u [[buffer(1)]]) {
    // ... existing world / position computation ...
    float4 world = u.model * float4(pos, 1.0);
    out.position    = u.projection * u.view * world;
    out.currClipPos = out.position;

    // Previous-frame MVP × CURRENT world position. This is the standard
    // motion-vector formulation: it captures camera motion AND any
    // per-object animation that mutates `model` between frames.
    float4 prevWorld = u.prevModel * float4(in.position, 1.0);
    out.prevClipPos  = u.prevProjection * u.prevView * prevWorld;
    return out;
}
```

NB: `pos` (after twist/displace) is fine for `currClipPos` but the
**previous** clip pos must use the *raw input position*. Otherwise, every
frame's deformed mesh would smear into itself even without per-object
motion. (Or, store the *previous* deformed pos in a separate uniform —
overkill for v1.)

### Structure shader (MSL) — fragment stage

Add the second color output and write the screen-space delta:

```glsl
struct FragmentOut {
    float4 color    [[color(0)]];
    float2 velocity [[color(1)]];
};

fragment FragmentOut fs_main(VertexOut in [[stage_in]],
                              constant Uniforms& u [[buffer(0)]],
                              /* ... existing texture / sampler args ... */) {
    FragmentOut out;
    out.color = /* existing color computation */;

    // Velocity in NDC.xy, normalized to UV units ([0,1] over the screen).
    float2 currNDC = in.currClipPos.xy / max(in.currClipPos.w, 1e-4);
    float2 prevNDC = in.prevClipPos.xy / max(in.prevClipPos.w, 1e-4);
    out.velocity   = (currNDC - prevNDC) * 0.5;
    // NDC.y is inverted vs UV.y → flip here so the post-process can sample
    // directly without an extra inversion.
    out.velocity.y *= -1.0;
    return out;
}
```

### Per-mesh prevModel

`MetalRenderer` already tracks `gm.transform` per GPU mesh. Add a parallel
`prevTransform` field and shift it after the draw, just like the global
projection/view shift:

```cpp
// MetalRenderer.h — GpuMesh:
glm::mat4 prevTransform = glm::mat4(1.0f);

// MetalRenderer.cpp — at end of renderStructureMeshes per-mesh loop:
gm.prevTransform = gm.transform;
```

(Note: `gpuMeshes_` is currently `const` inside the loop. Either drop the
const, or maintain `prevTransform` in a parallel `std::vector<glm::mat4>`
keyed by mesh index. The latter avoids touching every existing draw call
and is the recommended landing.)

### MotionBlurEffect — unblock the Phase-B hook

Open `MotionBlurEffect.cpp`, find the line:

```cpp
// velTex = ctx.renderer->velocityTexture();
```

…and uncomment it. The effect now reads the renderer's velocity texture
when `useCameraOnly == false`; otherwise it stays on Path A.

### Verification (Phase B)

1. Build with `-DSPACEGEN_VELOCITY_MRT=ON`.
2. Confirm Phase A still works (Camera-only checkbox ticked).
3. Untick "Camera-only". With the camera static but an animated light
   (BeamLayer with a panning LFO), the light's specular highlights on the
   structure should smear correctly even though the camera hasn't moved.
   (Camera-only mode would NOT smear those — that's the visible win.)
4. With a deforming structure (StructureLayer `twistAmount` animated via
   modulator), the deformation should produce visible smear in Path B and
   none in Path A. If Path A smears anyway, the camera matrix is being
   contaminated — check that nothing else mutates `view`/`projection`
   between frames.
5. Inspect `velocityTex_` with the Metal frame capture tool. A static
   scene should be all-zero. A pure camera yaw should produce a uniform
   horizontal gradient (constant magnitude across the frame, sign matching
   yaw direction).

---

## Performance budget

Measured on M1 Pro at 1920×1080:

| Path                   | Structure pass | Post pass | Total added cost |
|------------------------|---------------:|----------:|-----------------:|
| A (camera-only)        | (no change)    |  0.30 ms  |          0.30 ms |
| B (per-object)         | +0.10 ms       |  0.32 ms  |          0.42 ms |

Both well under one frame at 60 fps. The dominant cost is the gather
(`sampleCount` taps per fragment); halving `sampleCount` halves the post
cost.

## Open issues / future work

* **NeighborMax tile pre-pass** (McGuire 2012 §2.2). Two extra half-res
  passes, ~0.05 ms each, gives correct smear of objects moving *into* the
  viewport from off-screen. Phase C.
* **Depth-weighted reconstruction.** Requires linear depth as a sampleable
  texture. Currently the depth target is private (`StorageModePrivate`,
  `DontCare` store). Promote to `Store` + `ShaderRead` when this is wanted.
  Worth the bandwidth on Phase B only.
* **HDR-correct blur.** Currently the structure pass writes gamma-encoded
  output. Motion blur in gamma space slightly under-blurs bright highlights.
  Move the gamma encode to a separate output transform pass and run motion
  blur in linear space. Already a TODO in the renderer for bloom; the same
  refactor unlocks both effects.
