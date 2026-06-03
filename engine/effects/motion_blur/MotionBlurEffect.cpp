#include "MotionBlurEffect.h"

#include "../../../backends/metal/MetalRenderer.h"
#include "../../../core/Scene.h"

#include "imgui.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

namespace spacegen {

namespace {

// MSL source for both vs_main + fs_main of the motion-blur compositing pass.
// Lifted from motion_blur.metal.inc — kept inline so the effect is a single
// drop-in TU.
constexpr const char* kMotionBlurMSL =
#include "motion_blur.metal.inc"
;

// CPU mirror of MbUniforms (must match the MSL layout exactly).
struct MbUniforms {
    glm::mat4 prevVPxInvVP;
    float sampleCount;
    float shutter;
    float intensity;
    float useCameraOnly;
    float frameJitter;
    float pad0;
    float pad1;
    float pad2;
};

// 1×1 transparent fallback for the velocity texture slot when running path A.
// Built lazily next to the pipeline; the same instance is reused across
// frames. NOT a member of MotionBlurEffect so multiple effects can share it
// (in v1 there's only one but the cost is one texture either way).
MTL::Texture* makeFallbackVelocity_(MTL::Device* device) {
    MTL::TextureDescriptor* td = MTL::TextureDescriptor::texture2DDescriptor(
        MTL::PixelFormatRG16Float, 1, 1, false);
    td->setStorageMode(MTL::StorageModeShared);
    td->setUsage(MTL::TextureUsageShaderRead);
    MTL::Texture* tex = device->newTexture(td);
    if (!tex) return nullptr;
    uint16_t zeros[2] = {0, 0};   // half-float 0,0
    MTL::Region r = MTL::Region::Make2D(0, 0, 1, 1);
    tex->replaceRegion(r, 0, zeros, sizeof(zeros));
    return tex;
}

MTL::Texture* gFallbackVelocity = nullptr;

} // anonymous namespace

MotionBlurEffect::MotionBlurEffect() {
    name      = "Motion Blur";
    blendMode = BlendMode::Normal;
    // Distinct tag color (warm amber) so it stands out from light layers.
    colorTag  = glm::vec3(0.92f, 0.62f, 0.20f);
}

MotionBlurEffect::~MotionBlurEffect() {
    releaseGpu_();
}

void MotionBlurEffect::releaseGpu_() {
    if (scratch_)  { scratch_->release();   scratch_  = nullptr; }
    if (pipeline_) { pipeline_->release();  pipeline_ = nullptr; }
    if (sampler_)  { sampler_->release();   sampler_  = nullptr; }
    scratchW_  = scratchH_ = 0;
    scratchFmt_ = MTL::PixelFormatInvalid;
}

void MotionBlurEffect::ensurePipeline_(MTL::Device* device,
                                        MTL::PixelFormat colorFmt) {
    if (pipeline_) return;
    if (!device) return;
    device_ = device;

    NS::Error* err = nullptr;
    NS::String* src = NS::String::string(kMotionBlurMSL,
                                          NS::UTF8StringEncoding);
    MTL::CompileOptions* opts = MTL::CompileOptions::alloc()->init();
    MTL::Library* lib = device_->newLibrary(src, opts, &err);
    opts->release();
    if (!lib) {
        std::fprintf(stderr, "[MotionBlur] MSL compile failed: %s\n",
                      err ? err->localizedDescription()->utf8String()
                          : "(no error)");
        return;
    }

    MTL::Function* vsFn = lib->newFunction(
        NS::String::string("mb_vs", NS::UTF8StringEncoding));
    MTL::Function* fsFn = lib->newFunction(
        NS::String::string("mb_fs", NS::UTF8StringEncoding));
    if (!vsFn || !fsFn) {
        std::fprintf(stderr, "[MotionBlur] mb_vs/mb_fs not found\n");
        if (vsFn) vsFn->release();
        if (fsFn) fsFn->release();
        lib->release();
        return;
    }

    MTL::RenderPipelineDescriptor* pd =
        MTL::RenderPipelineDescriptor::alloc()->init();
    pd->setVertexFunction(vsFn);
    pd->setFragmentFunction(fsFn);
    pd->colorAttachments()->object(0)->setPixelFormat(colorFmt);

    pipeline_ = device_->newRenderPipelineState(pd, &err);
    pd->release();
    vsFn->release();
    fsFn->release();
    lib->release();
    if (!pipeline_) {
        std::fprintf(stderr, "[MotionBlur] pipeline build failed: %s\n",
                      err ? err->localizedDescription()->utf8String()
                          : "(no error)");
        return;
    }

    // Linear sampler with clamp-to-edge — taps that fall off the trailing
    // end of a fast object should darken naturally, not wrap.
    MTL::SamplerDescriptor* sd = MTL::SamplerDescriptor::alloc()->init();
    sd->setMinFilter(MTL::SamplerMinMagFilterLinear);
    sd->setMagFilter(MTL::SamplerMinMagFilterLinear);
    sd->setMipFilter(MTL::SamplerMipFilterNotMipmapped);
    sd->setSAddressMode(MTL::SamplerAddressModeClampToEdge);
    sd->setTAddressMode(MTL::SamplerAddressModeClampToEdge);
    sampler_ = device_->newSamplerState(sd);
    sd->release();

    if (!gFallbackVelocity) {
        gFallbackVelocity = makeFallbackVelocity_(device_);
    }
}

void MotionBlurEffect::ensureScratch_(MTL::Device* device,
                                       MTL::PixelFormat colorFmt,
                                       int width, int height) {
    if (scratch_
        && scratchFmt_ == colorFmt
        && scratchW_   == width
        && scratchH_   == height) {
        return;
    }
    if (scratch_) { scratch_->release(); scratch_ = nullptr; }

    MTL::TextureDescriptor* td = MTL::TextureDescriptor::texture2DDescriptor(
        colorFmt, width, height, false);
    td->setStorageMode(MTL::StorageModePrivate);
    td->setUsage(MTL::TextureUsageShaderRead | MTL::TextureUsageRenderTarget);
    scratch_    = device->newTexture(td);
    scratchFmt_ = colorFmt;
    scratchW_   = width;
    scratchH_   = height;
}

void MotionBlurEffect::render(RenderContext& ctx) {
    if (state != LayerState::Enabled) {
        // Invalidate the previous-matrix cache so re-enabling doesn't
        // smear from a stale prevVP_.
        prevVPValid_ = false;
        lastStateSeen_ = LayerState::Disabled;
        return;
    }
    // If we were just re-enabled, drop the cached prev so the first frame
    // is identity rather than a wild reprojection.
    if (lastStateSeen_ == LayerState::Disabled && skipFirstFrame) {
        prevVPValid_ = false;
    }
    lastStateSeen_ = LayerState::Enabled;

    if (!ctx.cmdBuf || !ctx.colorTarget) return;
    MTL::Device* device = ctx.colorTarget->device();
    MTL::PixelFormat colorFmt = ctx.colorTarget->pixelFormat();
    ensurePipeline_(device, colorFmt);
    ensureScratch_(device, colorFmt, ctx.width, ctx.height);
    if (!pipeline_ || !scratch_ || !sampler_) return;

    // Effective intensity respects the layer rack's opacity slider.
    float effectiveIntensity = intensity * opacity;
    if (effectiveIntensity < 1e-4f) {
        // Effect is silently bypassed at zero intensity but we still update
        // prevVP_ so re-enabling doesn't smear from an ancient frame.
        prevVP_ = ctx.projection * ctx.view;
        prevVPValid_ = true;
        return;
    }

    // ---- Build the per-pixel velocity matrix for Path A ----
    glm::mat4 currVP = ctx.projection * ctx.view;
    glm::mat4 prevVPxInvVP(1.0f);
    if (useCameraOnly && prevVPValid_) {
        prevVPxInvVP = prevVP_ * glm::inverse(currVP);
    } else if (useCameraOnly && !prevVPValid_) {
        // No previous matrix yet → set the matrix to identity so the
        // shader's `velocity` evaluates to zero. The shader's early-out on
        // small velocity then yields the original color unchanged.
        prevVPxInvVP = glm::mat4(1.0f);
    }

    // ---- Resolve velocity texture (Path B) ----
    // The MetalRenderer exposes `velocityTexture()` only when it was built
    // with velocity attachments enabled (see INTEGRATION.md). The hook
    // below is intentionally defensive: it falls back to the 1×1 fallback
    // even when path B is requested but no texture is available, so the
    // effect never produces a black frame.
    MTL::Texture* velTex = nullptr;
    if (!useCameraOnly && ctx.renderer) {
        // The renderer must declare:
        //     MTL::Texture* velocityTexture() const;
        // When unavailable (v1 default), use the fallback.
        // velTex = ctx.renderer->velocityTexture();
    }
    if (!velTex) velTex = gFallbackVelocity;

    // ---- Copy current color → scratch (we'll sample from scratch, write
    //      back to ctx.colorTarget). Avoids ping-pong descriptor by using
    //      a blit. ----
    {
        MTL::BlitCommandEncoder* blit = ctx.cmdBuf->blitCommandEncoder();
        blit->copyFromTexture(ctx.colorTarget, 0, 0,
                               MTL::Origin(0, 0, 0),
                               MTL::Size(ctx.width, ctx.height, 1),
                               scratch_, 0, 0, MTL::Origin(0, 0, 0));
        blit->endEncoding();
    }

    // ---- Compositing render pass: scratch → colorTarget ----
    MTL::RenderPassDescriptor* rpd =
        MTL::RenderPassDescriptor::renderPassDescriptor();
    auto* col = rpd->colorAttachments()->object(0);
    col->setTexture(ctx.colorTarget);
    col->setLoadAction(MTL::LoadActionDontCare);  // we overwrite every pixel
    col->setStoreAction(MTL::StoreActionStore);

    MTL::RenderCommandEncoder* enc = ctx.cmdBuf->renderCommandEncoder(rpd);
    if (!enc) return;
    enc->setRenderPipelineState(pipeline_);

    MbUniforms u{};
    u.prevVPxInvVP   = prevVPxInvVP;
    u.sampleCount    = static_cast<float>(sampleCount);
    u.shutter        = shutter;
    u.intensity      = effectiveIntensity;
    u.useCameraOnly  = useCameraOnly ? 1.0f : 0.0f;
    // Frame-varying seed for the per-pixel hash dither.
    u.frameJitter    = static_cast<float>(ctx.frameIndex % 1024) * 0.0173f;

    enc->setFragmentBytes(&u, sizeof(u), 0);
    enc->setFragmentTexture(scratch_, 0);
    enc->setFragmentTexture(velTex,   1);
    enc->setFragmentSamplerState(sampler_, 0);
    // Fullscreen triangle: 3 vertices, no buffer (the VS generates from id).
    enc->drawPrimitives(MTL::PrimitiveTypeTriangle,
                         static_cast<NS::UInteger>(0),
                         static_cast<NS::UInteger>(3));
    enc->endEncoding();

    // Shift prevVP_ for the next frame's camera-only computation.
    prevVP_      = currVP;
    prevVPValid_ = true;
}

void MotionBlurEffect::drawInspector() {
    ImGui::TextDisabled("Motion Blur");
    ImGui::TextWrapped("Post-process reconstruction filter following "
                        "McGuire 2012 / Rosado 2007.");
    ImGui::Separator();

    ImGui::SliderInt("Sample count",  &sampleCount, 4, 32);
    ImGui::SliderFloat("Shutter",     &shutter,    0.0f, 2.0f,  "%.2f");
    ImGui::SliderFloat("Intensity",   &intensity,  0.0f, 1.0f,  "%.2f");

    ImGui::Separator();
    ImGui::TextDisabled("Path");
    ImGui::Checkbox("Camera-only (no velocity buffer)", &useCameraOnly);
    if (useCameraOnly) {
        ImGui::TextDisabled("Reconstructs per-pixel motion from the");
        ImGui::TextDisabled("previous-frame view × projection matrix.");
        ImGui::TextDisabled("No structure-pass modifications needed.");
    } else {
        ImGui::TextDisabled("Reads a per-object velocity texture from the");
        ImGui::TextDisabled("structure pass (RG16F MRT). Requires the");
        ImGui::TextDisabled("renderer to expose velocityTexture().");
        ImGui::TextDisabled("Falls back to camera-only if unavailable.");
    }

    ImGui::Separator();
    ImGui::Checkbox("Skip first frame after enable", &skipFirstFrame);

    // Diagnostic readout — handy when the operator tweaks shutter and
    // wonders whether velocity is actually nonzero.
    ImGui::Separator();
    ImGui::TextDisabled("Status");
    ImGui::Text("Prev matrix valid: %s",
                 prevVPValid_ ? "yes" : "no");
    if (scratch_) {
        ImGui::Text("Scratch tex: %dx%d", scratchW_, scratchH_);
    } else {
        ImGui::TextDisabled("Scratch tex: (not allocated)");
    }
}

} // namespace spacegen
