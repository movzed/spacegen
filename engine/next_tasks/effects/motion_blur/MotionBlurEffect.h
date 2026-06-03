#pragma once
// MotionBlurEffect — post-process motion blur for SpaceGen.
//
// Implements the McGuire 2012 / Rosado 2007 reconstruction filter against
// the offscreen color target. Two paths:
//   - Camera-only (default):    no velocity G-buffer needed. The effect
//                                 reconstructs camera-frame delta from the
//                                 previous and current projection × view
//                                 matrices (stored across frames inside the
//                                 effect itself) and gathers along that
//                                 direction.
//   - Per-object (opt-in):       requires the structure pass to write a
//                                 RG16F velocity attachment (see
//                                 INTEGRATION.md). When `useCameraOnly` is
//                                 false and the renderer exposes a non-null
//                                 velocity texture, the shader reads it
//                                 per-fragment.
//
// The class owns one transient scratch color texture (ping-pong source)
// recreated only when the color target's dimensions change. All other state
// is uniforms.

#include "../../../core/Layer.h"

#include <Metal/Metal.hpp>

#include <glm/glm.hpp>
#include <glm/mat4x4.hpp>

namespace spacegen {

class MotionBlurEffect : public ILayer {
public:
    MotionBlurEffect();
    ~MotionBlurEffect() override;

    MotionBlurEffect(const MotionBlurEffect&)            = delete;
    MotionBlurEffect& operator=(const MotionBlurEffect&) = delete;

    LayerKind   kind()     const override { return LayerKind::Effect; }
    const char* typeName() const override { return "Motion Blur"; }
    void        render(RenderContext& ctx) override;
    void        drawInspector() override;

    // ---- Inspector-controlled state (all live; safe to write from UI) ----

    // Number of color taps gathered along the velocity vector. 4 is chunky,
    // 16 is the production sweet spot, 32 looks silky on cinema cuts but
    // doubles fragment cost. Clamped to [4, 32] in the shader.
    int    sampleCount = 16;

    // Multiplier on the reconstructed velocity. 0 disables; 1 is a
    // physically plausible 360° shutter at 60 fps; 2 exaggerates for stage
    // effects. NB: this is *per-frame* velocity, not per-second — the
    // shutter convention follows McGuire 2012.
    float  shutter = 1.0f;

    // Mix between the original color (0) and the smeared version (1).
    // Multiplied by `opacity` at submit time, so the layer rack's opacity
    // slider fades the effect linearly.
    float  intensity = 1.0f;

    // Path selector. When true (default), runs the camera-only path and
    // ignores the renderer's velocity texture even if available. When
    // false, requests the renderer's velocity texture; if the renderer
    // doesn't expose one, the effect transparently falls back to camera-
    // only so it never breaks the frame.
    bool   useCameraOnly = true;

    // Optional: ignore the FIRST frame (no previous matrix yet, would
    // smear toward NDC origin from anywhere). Defaults true; clears the
    // "previous" cache when the layer is disabled and re-enabled so the
    // operator gets a clean transition.
    bool   skipFirstFrame = true;

private:
    // GPU pipeline init (lazy on first render — needs ctx.colorTarget's
    // device, which we don't have at construction time).
    void  ensurePipeline_(MTL::Device* device,
                           MTL::PixelFormat colorFmt);

    // Reallocate the scratch ping-pong texture if size or format changed.
    void  ensureScratch_(MTL::Device* device,
                          MTL::PixelFormat colorFmt,
                          int width, int height);

    void  releaseGpu_();

    // ---- GPU resources ----
    MTL::Device*               device_         = nullptr;   // non-owning
    MTL::RenderPipelineState*  pipeline_       = nullptr;
    MTL::SamplerState*         sampler_        = nullptr;
    MTL::Texture*              scratch_        = nullptr;
    MTL::PixelFormat           scratchFmt_     = MTL::PixelFormatInvalid;
    int                        scratchW_       = 0;
    int                        scratchH_       = 0;

    // ---- Camera-only path state ----
    // The view × projection matrix of the previous frame. Recomputed at
    // end-of-render so the next frame can read it as the "before".
    glm::mat4  prevVP_           = glm::mat4(1.0f);
    bool       prevVPValid_      = false;
    LayerState lastStateSeen_    = LayerState::Enabled;

    // Cached inspector-disabled flag — used to detect transitions and
    // invalidate prevVP_ so re-enabling doesn't smear from a stale matrix.
};

} // namespace spacegen
