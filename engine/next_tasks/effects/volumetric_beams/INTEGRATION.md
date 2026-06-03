# INTEGRATION — wiring VolumetricBeamLayer into MetalRenderer

Five concrete changes, all in `engine/backends/metal/MetalRenderer.{h,cpp}`,
plus the scene-bus registration (`Scene.cpp` / wherever the operator adds
layers). No CMakeLists changes needed beyond glob-matching the new
`fx/volumetrics/*.cpp` (which the existing globs already handle).

## 1. Make the depth texture readable as a shader resource

`MetalRenderer::onResize` currently builds the depth texture with
`MTL::TextureUsageRenderTarget` only and `StorageModePrivate`. We need to
add `MTL::TextureUsageShaderRead` so the volumetric fragment shader can
sample it.

**File**: `MetalRenderer.cpp`, function `onResize`.

```cpp
// Before
td->setStorageMode(MTL::StorageModePrivate);
td->setUsage(MTL::TextureUsageRenderTarget);

// After
td->setStorageMode(MTL::StorageModePrivate);
td->setUsage(MTL::TextureUsageRenderTarget
             | MTL::TextureUsageShaderRead);
```

Private storage stays private — sampling a private texture in the same
command buffer is legal on Apple Silicon (the tile memory is hoisted via
the encoder dependency). We use `depth2d<float>` in the shader, which is
the standard MSL declaration for sampling a `Depth32Float` resource.

## 2. Store the depth attachment instead of discarding it

The structure pass currently does `setStoreAction(MTL::StoreActionDontCare)`
on the depth attachment. The volumetric needs the depth values intact AFTER
the structure pass finishes.

**File**: `MetalRenderer.cpp`, `renderStructureMeshes` (around the
`depthAttachment` setup).

```cpp
// Before
depthAttachment->setStoreAction(MTL::StoreActionDontCare);

// After
depthAttachment->setStoreAction(MTL::StoreActionStore);
```

Cost: writing the depth tile back to memory at the end of the structure
pass. On M1 Max @1080p ~D32 the depth target is ~8 MB — store cost is well
under 0.1 ms with the tile-based deferred renderer.

## 3. Expose the depth texture to layers

Volumetric layers need to bind the depth texture. The cleanest path is to
add an accessor; layers already hold a pointer to the renderer through
`ctx.renderer`.

**File**: `MetalRenderer.h`, public accessors section (next to `projection()`
/ `view()`).

```cpp
// Returns the depth texture written by the most recent structure pass.
// Caller MUST treat it as read-only and MUST NOT bind it as both a
// render-attachment and a shader resource in the same encoder.
MTL::Texture* depthTexture() const { return depthTex_; }
```

## 4. Add the volumetric pipeline + uniforms type

Two additions to `MetalRenderer.h`:

```cpp
// (top of class, public section)
struct VolumetricSpot {
    glm::vec4 posIntensity;
    glm::vec4 dirRange;
    glm::vec4 colorInner;
    glm::vec4 outerMul;
};
struct VolumetricUniforms {
    const VolumetricSpot* spots;    // borrowed; renderer copies into argbuf
    int                   spotCount;
    float                 density;
    float                 anisotropy;
    int                   sampleCount;
    float                 jitterStrength;
    glm::vec3             tint;
    bool                  beerLambert;
    float                 layerOpacity;
};

void renderVolumetricBeams(RenderContext& ctx, const VolumetricUniforms& u);
```

And the matching private members:

```cpp
MTL::RenderPipelineState* volBeamPipeline_ = nullptr;
MTL::DepthStencilState*   volBeamDepthState_ = nullptr;  // depth disabled
void buildVolumetricBeamPipeline();
```

## 5. Build the volumetric pipeline at startup

In `MetalRenderer::MetalRenderer(...)` constructor, after
`buildDefaultTexturesAndSampler()` and `buildPipeline()`, add:

```cpp
buildVolumetricBeamPipeline();
```

And in the destructor:

```cpp
if (volBeamPipeline_)   volBeamPipeline_->release();
if (volBeamDepthState_) volBeamDepthState_->release();
```

The body of `buildVolumetricBeamPipeline()` is:

```cpp
void MetalRenderer::buildVolumetricBeamPipeline() {
    NS::Error* err = nullptr;

    // The MSL source is from volumetric.metal.inc, embedded as a raw-string
    // constant. Copy the file contents between R"MSL( ... )MSL" delimiters.
    static constexpr const char* kVolumetricBeamMSL = R"MSL(
        // <paste the entire contents of volumetric.metal.inc here>
    )MSL";

    NS::String* src = NS::String::string(kVolumetricBeamMSL,
                                          NS::UTF8StringEncoding);
    MTL::CompileOptions* opts = MTL::CompileOptions::alloc()->init();
    MTL::Library* lib = device_->newLibrary(src, opts, &err);
    opts->release();
    if (!lib) {
        std::string msg = "VolBeam MSL compile failed: ";
        if (err) msg += err->localizedDescription()->utf8String();
        throw std::runtime_error(msg);
    }

    MTL::Function* vsFn = lib->newFunction(
        NS::String::string("vb_vs", NS::UTF8StringEncoding));
    MTL::Function* fsFn = lib->newFunction(
        NS::String::string("vb_fs", NS::UTF8StringEncoding));
    if (!vsFn || !fsFn) {
        if (vsFn) vsFn->release();
        if (fsFn) fsFn->release();
        lib->release();
        throw std::runtime_error("VolBeam: vb_vs or vb_fs not found");
    }

    MTL::RenderPipelineDescriptor* pd =
        MTL::RenderPipelineDescriptor::alloc()->init();
    pd->setVertexFunction(vsFn);
    pd->setFragmentFunction(fsFn);
    auto* ca = pd->colorAttachments()->object(0);
    ca->setPixelFormat(colorFormat_);
    // Additive blend — we composite the new in-scattering on top of the
    // existing structure color without overwriting it.
    ca->setBlendingEnabled(true);
    ca->setRgbBlendOperation(MTL::BlendOperationAdd);
    ca->setAlphaBlendOperation(MTL::BlendOperationAdd);
    ca->setSourceRGBBlendFactor(MTL::BlendFactorOne);
    ca->setDestinationRGBBlendFactor(MTL::BlendFactorOne);
    ca->setSourceAlphaBlendFactor(MTL::BlendFactorOne);
    ca->setDestinationAlphaBlendFactor(MTL::BlendFactorOne);
    // We must declare the depth format even though we don't write — Metal
    // wants the pipeline's depth format to match the encoder's attachment.
    pd->setDepthAttachmentPixelFormat(MTL::PixelFormatDepth32Float);

    volBeamPipeline_ = device_->newRenderPipelineState(pd, &err);
    pd->release();
    vsFn->release();
    fsFn->release();
    lib->release();

    if (!volBeamPipeline_) {
        std::string msg = "VolBeam pipeline build failed: ";
        if (err) msg += err->localizedDescription()->utf8String();
        throw std::runtime_error(msg);
    }

    // Depth state: NO depth test, NO depth write. The volumetric does its
    // own occlusion via sceneDepth sampling in the fragment shader.
    MTL::DepthStencilDescriptor* dsd =
        MTL::DepthStencilDescriptor::alloc()->init();
    dsd->setDepthCompareFunction(MTL::CompareFunctionAlways);
    dsd->setDepthWriteEnabled(false);
    volBeamDepthState_ = device_->newDepthStencilState(dsd);
    dsd->release();
}
```

## 6. Implement renderVolumetricBeams

This is the per-frame entry point that VolumetricBeamLayer calls.

```cpp
void MetalRenderer::renderVolumetricBeams(RenderContext& ctx,
                                            const VolumetricUniforms& vu)
{
    if (!ctx.cmdBuf || !ctx.colorTarget) return;
    if (!volBeamPipeline_) return;
    if (!depthTex_) return;
    if (vu.spotCount <= 0) return;

    // Begin a render pass that LOADS the existing color (so we composite
    // over the structure pass) and uses the structure pass's depth as a
    // SHADER input (so we can't also use it as the encoder's depth
    // attachment for the same pass). The trick: we bind the depth attachment
    // with LoadAction=Load + StoreAction=Store on a DIFFERENT depth target?
    // No — there isn't a second depth target. Instead, we omit the depth
    // attachment entirely, since the pipeline's depth state has
    // CompareFunction=Always and WriteEnabled=false.
    //
    // BUT: the pipeline's depth-attachment-pixel-format must still match
    // the encoder. To square this, we configure the pipeline with
    // PixelFormatInvalid for the depth attachment and OMIT the depth
    // attachment in the render pass descriptor.

    MTL::RenderPassDescriptor* rpd =
        MTL::RenderPassDescriptor::renderPassDescriptor();
    auto* colorAttachment = rpd->colorAttachments()->object(0);
    colorAttachment->setTexture(ctx.colorTarget);
    colorAttachment->setLoadAction(MTL::LoadActionLoad);   // keep structure pass
    colorAttachment->setStoreAction(MTL::StoreActionStore);

    MTL::RenderCommandEncoder* enc = ctx.cmdBuf->renderCommandEncoder(rpd);
    if (!enc) return;

    enc->setRenderPipelineState(volBeamPipeline_);
    enc->setDepthStencilState(volBeamDepthState_);
    enc->setCullMode(MTL::CullModeNone);

    // ---- Build the uniform block ----
    struct GpuVolUniforms {
        glm::mat4 invViewProj;
        glm::vec4 cameraWorldPos;
        glm::vec4 params0;     // density, g, sampleCount, jitterStrength
        glm::vec4 params1;     // tint.rgb, beerLambert
        glm::vec4 counts;      // spotCount, layerOpacity, resW, resH
        struct GpuSpot {
            glm::vec4 posIntensity;
            glm::vec4 dirRange;
            glm::vec4 colorInner;
            glm::vec4 outerMul;
        } spots[32];
    };

    GpuVolUniforms gu{};
    glm::mat4 vp = ctx.projection * ctx.view;
    gu.invViewProj    = glm::inverse(vp);
    // ctx.cameraWorldPos.w stores the near-clip plane distance so the shader
    // can clamp the ray origin. ctx already has cameraWorldPos as vec3;
    // we encode the near separately via scene's camera.
    float camNear = ctx.scene ? ctx.scene->camera.clipStart : 0.1f;
    gu.cameraWorldPos = glm::vec4(ctx.cameraWorldPos, camNear);
    gu.params0 = glm::vec4(vu.density,
                            vu.anisotropy,
                            static_cast<float>(vu.sampleCount),
                            vu.jitterStrength);
    gu.params1 = glm::vec4(vu.tint, vu.beerLambert ? 1.0f : 0.0f);
    gu.counts  = glm::vec4(static_cast<float>(vu.spotCount),
                            vu.layerOpacity,
                            static_cast<float>(ctx.width),
                            static_cast<float>(ctx.height));
    const int n = std::min(vu.spotCount, 32);
    for (int i = 0; i < n; ++i) {
        gu.spots[i].posIntensity = vu.spots[i].posIntensity;
        gu.spots[i].dirRange     = vu.spots[i].dirRange;
        gu.spots[i].colorInner   = vu.spots[i].colorInner;
        gu.spots[i].outerMul     = vu.spots[i].outerMul;
    }

    enc->setFragmentBytes(&gu, sizeof(gu), 0);
    enc->setFragmentTexture(ctx.colorTarget, 0);   // sceneColor
    enc->setFragmentTexture(depthTex_,       1);   // sceneDepth
    enc->setFragmentSamplerState(linearSampler_, 0);

    // Two-triangle quad — 6 vertices, no buffer.
    enc->drawPrimitives(MTL::PrimitiveTypeTriangle,
                         static_cast<NS::UInteger>(0),
                         static_cast<NS::UInteger>(6));

    enc->endEncoding();
}
```

### Important nuance — `ctx.colorTarget` bound as both render target and shader resource

Metal allows reading from a texture you're also rendering to **only if you
use a framebuffer-fetch pattern** (`[[color(0)]]` in MSL) or you split into
separate passes. The simple way is the latter:

- **Option A** (chosen): copy `ctx.colorTarget` into a scratch texture at
  the start of `renderVolumetricBeams`, sample the scratch, write the
  composited result back to `ctx.colorTarget`. Adds ~0.1 ms blit.

- **Option B**: use programmable blending. Declare the fragment shader's
  output as `[[color(0)]]` with `read_color = true`. Works on Apple Silicon
  but requires the pipeline state to have `setFragmentFetchEnabled(true)`
  on the color attachment.

For initial integration, go with Option A. The blit is:

```cpp
// Lazily-allocated scratch texture, same dimensions as colorTarget.
if (!volScratchTex_ ||
    volScratchTex_->width()  != ctx.colorTarget->width() ||
    volScratchTex_->height() != ctx.colorTarget->height()) {
    if (volScratchTex_) volScratchTex_->release();
    MTL::TextureDescriptor* td = MTL::TextureDescriptor::texture2DDescriptor(
        colorFormat_,
        ctx.colorTarget->width(),
        ctx.colorTarget->height(),
        false);
    td->setStorageMode(MTL::StorageModePrivate);
    td->setUsage(MTL::TextureUsageShaderRead
                | MTL::TextureUsageRenderTarget);
    volScratchTex_ = device_->newTexture(td);
}

MTL::BlitCommandEncoder* blit = ctx.cmdBuf->blitCommandEncoder();
blit->copyFromTexture(ctx.colorTarget, 0, 0,
                       MTL::Origin(0, 0, 0),
                       MTL::Size(ctx.colorTarget->width(),
                                  ctx.colorTarget->height(), 1),
                       volScratchTex_, 0, 0,
                       MTL::Origin(0, 0, 0));
blit->endEncoding();

// Then bind volScratchTex_ (not ctx.colorTarget) as the sceneColor SRV.
enc->setFragmentTexture(volScratchTex_, 0);
```

Add the matching member + cleanup:

```cpp
MTL::Texture* volScratchTex_ = nullptr;
// destructor: if (volScratchTex_) volScratchTex_->release();
```

## 7. Bus ordering — where to put the layer

A volumetric layer must appear **above** StructureLayer in the bus (so it
runs after StructureLayer in the bottom-to-top traversal). The standard
operator workflow is:

```
[7] Bloom            (Effect)
[6] Volumetric Beams (Effect)   <-- new layer goes here
[5] Spot back-right  (Generator, BeamLayer)
[4] Spot front-left  (Generator, BeamLayer)
[3] Spot front-right (Generator, BeamLayer)
[2] Ambient fill     (Generator, AmbientLightLayer)
[1] Structure        (Generator, StructureLayer)
```

The volumetric reads the same BeamLayers that the structure already
consumed for surface illumination — no extra wiring on the operator side.

## 8. Optional: declare it audio-bindable when ParameterRegistry lands

When `ParameterRegistry` is in place (per the parameter-graph-spacegen
skill), wrap `density`, `anisotropy`, and `tint` as `Parameter` objects
in the layer's constructor so the operator can right-click any of them
in the inspector and bind to an FFT band. For v1 they're plain fields,
matching the rest of the layer code.

## 9. Verification checklist

After integration, the following should hold:

- [ ] Density 0: app behaves exactly like before (no visible volumetrics,
      no perf regression). Verifiable with the layer enabled vs disabled.
- [ ] With density ~0.03 and one BeamLayer: a visible cone appears
      between the fixture and the structure, occluded correctly by the
      structure (no cone "behind" geometry).
- [ ] Density ~0.07 + g=0.7: dense, forward-scattering cones (Tomorrowland-
      mainstage look).
- [ ] g=-0.7: backscatter; cones brighter from the camera-toward-light
      direction.
- [ ] Multi-fixture rig (Arc 7, 120°): seven cones, each animated by its
      phase-shifted LFO.
- [ ] StructureLayer in `emitLightsOnly` mode + VolumetricBeamLayer:
      composited output (alpha tracks light) has the cones in alpha too.
- [ ] Sample count 16 vs 128: visible quality difference (less banding
      at 128), proportional perf hit.

## 10. Files touched summary

| file | change |
|---|---|
| `MetalRenderer.h` | add `VolumetricSpot`, `VolumetricUniforms`, `renderVolumetricBeams`, `depthTexture()`, `volBeamPipeline_`, `volBeamDepthState_`, `volScratchTex_` |
| `MetalRenderer.cpp` | depth texture `TextureUsageShaderRead`, depth `StoreActionStore`, `buildVolumetricBeamPipeline()`, `renderVolumetricBeams()`, destructor release |
| `Scene.cpp` (or wherever the bus is populated) | optionally seed an initial `VolumetricBeamLayer` for demos |
| (new) `engine/fx/volumetrics/VolumetricBeamLayer.{h,cpp,metal}` | the layer itself |

No CMakeLists changes if the build system globs `engine/**/*.{cpp,metal}`.
If it does not, add the new source files to the appropriate target.
