#include "BloomEffect.h"

#include "../../../backends/metal/MetalRenderer.h"

#include <Metal/Metal.hpp>
#include <Foundation/Foundation.hpp>

#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <string>

namespace spacegen {

namespace {

// MSL source — embedded as a raw-string literal so the C++ side has one
// translation unit and no extra build artifact. The on-disk copy of the
// same code lives in `bloom.metal.inc` next to this file (see README.md);
// keep the two in sync when editing. This is the canonical pattern in
// SpaceGen's MetalRenderer.cpp — shader source lives in a `kXxxMSL` C++
// string constant; the .inc file is the human-facing copy.
constexpr const char* kBloomMSLFull = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct VOut {
    float4 position [[position]];
    float2 uv;
};

vertex VOut vs_fullscreen(uint vid [[vertex_id]]) {
    VOut o;
    float2 p = float2( (vid << 1) & 2, vid & 2);
    o.position = float4(p * 2.0 - 1.0, 0.0, 1.0);
    o.uv = float2(p.x, 1.0 - p.y);
    return o;
}

struct ThresholdU { float4 params; };

fragment float4 fs_threshold(VOut in [[stage_in]],
                              texture2d<float> src [[texture(0)]],
                              sampler          smp [[sampler(0)]],
                              constant ThresholdU& u [[buffer(0)]])
{
    float3 color = src.sample(smp, in.uv).rgb;
    float  luma  = dot(color, float3(0.2126, 0.7152, 0.0722));
    float  threshold = u.params.x;
    float  knee      = max(u.params.y, 1e-4);
    float soft = clamp(luma - threshold + knee, 0.0, 2.0 * knee);
    soft       = (soft * soft) / (4.0 * knee);
    float hard = max(luma - threshold, 0.0);
    float boost = max(soft, hard) / max(luma, 1e-4);
    return float4(color * boost, 1.0);
}

struct DownU { float4 params; };

static float3 karisAverage(float3 a, float3 b, float3 c, float3 d) {
    float wa = 1.0 / (1.0 + dot(a, float3(0.2126, 0.7152, 0.0722)));
    float wb = 1.0 / (1.0 + dot(b, float3(0.2126, 0.7152, 0.0722)));
    float wc = 1.0 / (1.0 + dot(c, float3(0.2126, 0.7152, 0.0722)));
    float wd = 1.0 / (1.0 + dot(d, float3(0.2126, 0.7152, 0.0722)));
    float wsum = wa + wb + wc + wd;
    return (a * wa + b * wb + c * wc + d * wd) * (1.0 / wsum);
}

fragment float4 fs_downsample(VOut in [[stage_in]],
                               texture2d<float> src [[texture(0)]],
                               sampler          smp [[sampler(0)]],
                               constant DownU&  u   [[buffer(0)]])
{
    float2 t   = u.params.xy;
    float2 uv  = in.uv;
    bool   useKaris = u.params.z > 0.5;

    float3 a = src.sample(smp, uv + t * float2(-2.0,  2.0)).rgb;
    float3 b = src.sample(smp, uv + t * float2( 0.0,  2.0)).rgb;
    float3 c = src.sample(smp, uv + t * float2( 2.0,  2.0)).rgb;
    float3 d = src.sample(smp, uv + t * float2(-2.0,  0.0)).rgb;
    float3 e = src.sample(smp, uv                          ).rgb;
    float3 f = src.sample(smp, uv + t * float2( 2.0,  0.0)).rgb;
    float3 g = src.sample(smp, uv + t * float2(-2.0, -2.0)).rgb;
    float3 h = src.sample(smp, uv + t * float2( 0.0, -2.0)).rgb;
    float3 i = src.sample(smp, uv + t * float2( 2.0, -2.0)).rgb;
    float3 j = src.sample(smp, uv + t * float2(-1.0,  1.0)).rgb;
    float3 k = src.sample(smp, uv + t * float2( 1.0,  1.0)).rgb;
    float3 l = src.sample(smp, uv + t * float2(-1.0, -1.0)).rgb;
    float3 m = src.sample(smp, uv + t * float2( 1.0, -1.0)).rgb;

    float3 q0, q1, q2, q3, q4;
    if (useKaris) {
        q0 = karisAverage(j, k, l, m);
        q1 = karisAverage(a, b, d, e);
        q2 = karisAverage(b, c, e, f);
        q3 = karisAverage(d, e, g, h);
        q4 = karisAverage(e, f, h, i);
    } else {
        q0 = (j + k + l + m) * 0.25;
        q1 = (a + b + d + e) * 0.25;
        q2 = (b + c + e + f) * 0.25;
        q3 = (d + e + g + h) * 0.25;
        q4 = (e + f + h + i) * 0.25;
    }
    float3 result = q0 * 0.5 + (q1 + q2 + q3 + q4) * 0.125;
    return float4(max(result, float3(0.0)), 1.0);
}

struct UpU { float4 params; };

fragment float4 fs_upsample(VOut in [[stage_in]],
                             texture2d<float> src [[texture(0)]],
                             sampler          smp [[sampler(0)]],
                             constant UpU&    u   [[buffer(0)]])
{
    float2 t  = u.params.xy * u.params.z;
    float2 uv = in.uv;
    float3 a = src.sample(smp, uv + t * float2(-1.0,  1.0)).rgb;
    float3 b = src.sample(smp, uv + t * float2( 0.0,  1.0)).rgb;
    float3 c = src.sample(smp, uv + t * float2( 1.0,  1.0)).rgb;
    float3 d = src.sample(smp, uv + t * float2(-1.0,  0.0)).rgb;
    float3 e = src.sample(smp, uv                         ).rgb;
    float3 f = src.sample(smp, uv + t * float2( 1.0,  0.0)).rgb;
    float3 g = src.sample(smp, uv + t * float2(-1.0, -1.0)).rgb;
    float3 h = src.sample(smp, uv + t * float2( 0.0, -1.0)).rgb;
    float3 i = src.sample(smp, uv + t * float2( 1.0, -1.0)).rgb;
    float3 result = (a + c + g + i) * (1.0 / 16.0)
                  + (b + d + f + h) * (2.0 / 16.0)
                  +  e              * (4.0 / 16.0);
    return float4(result, 1.0);
}

struct CompU { float4 tintIntensity; };

fragment float4 fs_composite(VOut in [[stage_in]],
                              texture2d<float> scene [[texture(0)]],
                              texture2d<float> bloom [[texture(1)]],
                              sampler          smp   [[sampler(0)]],
                              constant CompU&  u     [[buffer(0)]])
{
    float3 s = scene.sample(smp, in.uv).rgb;
    float3 b = bloom.sample(smp, in.uv).rgb * u.tintIntensity.xyz;
    float  k = clamp(u.tintIntensity.w, 0.0, 2.0);
    float3 outRgb = mix(s, b, min(k, 1.0)) + b * max(k - 1.0, 0.0);
    return float4(outRgb, 1.0);
}
)MSL";

// Build a render pipeline state from an already-loaded library + a fragment
// function name. Vertex stage is always vs_fullscreen. `additive` selects
// a One+One color blend (used by the upsample pass) — all other passes
// use OpaqueWrite (StoreActionStore, no blend).
MTL::RenderPipelineState* buildPipeline(MTL::Device* device,
                                         MTL::Library* lib,
                                         const char* fragmentName,
                                         MTL::PixelFormat colorFormat,
                                         bool additive)
{
    if (!device || !lib) return nullptr;

    MTL::Function* vs = lib->newFunction(
        NS::String::string("vs_fullscreen", NS::UTF8StringEncoding));
    MTL::Function* fs = lib->newFunction(
        NS::String::string(fragmentName, NS::UTF8StringEncoding));
    if (!vs || !fs) {
        if (vs) vs->release();
        if (fs) fs->release();
        std::fprintf(stderr, "[BloomEffect] function not found: vs_fullscreen / %s\n",
                     fragmentName);
        return nullptr;
    }

    MTL::RenderPipelineDescriptor* pd =
        MTL::RenderPipelineDescriptor::alloc()->init();
    pd->setVertexFunction(vs);
    pd->setFragmentFunction(fs);
    auto* col = pd->colorAttachments()->object(0);
    col->setPixelFormat(colorFormat);
    if (additive) {
        col->setBlendingEnabled(true);
        col->setRgbBlendOperation(MTL::BlendOperationAdd);
        col->setAlphaBlendOperation(MTL::BlendOperationAdd);
        col->setSourceRGBBlendFactor(MTL::BlendFactorOne);
        col->setDestinationRGBBlendFactor(MTL::BlendFactorOne);
        col->setSourceAlphaBlendFactor(MTL::BlendFactorOne);
        col->setDestinationAlphaBlendFactor(MTL::BlendFactorOne);
    }

    NS::Error* err = nullptr;
    MTL::RenderPipelineState* pso = device->newRenderPipelineState(pd, &err);
    pd->release();
    vs->release();
    fs->release();
    if (!pso) {
        std::string msg = "[BloomEffect] pipeline build failed (";
        msg += fragmentName;
        msg += "): ";
        if (err) msg += err->localizedDescription()->utf8String();
        std::fprintf(stderr, "%s\n", msg.c_str());
    }
    return pso;
}

// Allocate a private texture for use as both a render target and a shader
// input (subsequent passes sample it). All bloom textures share the same
// pixel format as the scene's colorTarget.
MTL::Texture* allocSampleableTarget(MTL::Device* device,
                                     int width, int height,
                                     MTL::PixelFormat fmt)
{
    if (!device || width <= 0 || height <= 0) return nullptr;
    MTL::TextureDescriptor* td = MTL::TextureDescriptor::texture2DDescriptor(
        fmt, width, height, false);
    td->setStorageMode(MTL::StorageModePrivate);
    td->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
    return device->newTexture(td);
}

} // anonymous namespace

// ============================================================
// BloomEffect
// ============================================================

BloomEffect::BloomEffect() {
    name      = "Bloom";
    blendMode = BlendMode::Normal;        // we are a post-process, not blended
    colorTag  = glm::vec3(0.95f, 0.80f, 0.40f);
}

BloomEffect::~BloomEffect() {
    resetAll();
}

void BloomEffect::resetAll() {
    for (int i = 0; i < kMaxMips; ++i) {
        if (mips_[i]) { mips_[i]->release(); mips_[i] = nullptr; }
        mipW_[i] = mipH_[i] = 0;
    }
    if (compositeScratch_) { compositeScratch_->release(); compositeScratch_ = nullptr; }
    compositeW_ = compositeH_ = 0;

    if (psoThreshold_)  { psoThreshold_->release();  psoThreshold_  = nullptr; }
    if (psoDownsample_) { psoDownsample_->release(); psoDownsample_ = nullptr; }
    if (psoUpsample_)   { psoUpsample_->release();   psoUpsample_   = nullptr; }
    if (psoComposite_)  { psoComposite_->release();  psoComposite_  = nullptr; }
    if (sampler_)       { sampler_->release();       sampler_       = nullptr; }
    if (library_)       { library_->release();       library_       = nullptr; }

    device_            = nullptr;
    cachedPixelFormat_ = 0;
    cachedColorW_ = cachedColorH_ = 0;
}

bool BloomEffect::buildAll(MTL::Device* device, int colorPixelFormat) {
    if (!device) return false;
    // If we've already built for this device + pixel format, nothing to do.
    if (device_ == device
        && library_
        && psoThreshold_ && psoDownsample_ && psoUpsample_ && psoComposite_
        && cachedPixelFormat_ == colorPixelFormat) {
        return true;
    }
    // Otherwise wipe and rebuild — happens on first frame or if the
    // workstation's pixel format ever changes.
    resetAll();
    device_            = device;
    cachedPixelFormat_ = colorPixelFormat;

    // Compile MSL.
    NS::Error* err = nullptr;
    NS::String* src = NS::String::string(kBloomMSLFull, NS::UTF8StringEncoding);
    MTL::CompileOptions* opts = MTL::CompileOptions::alloc()->init();
    library_ = device->newLibrary(src, opts, &err);
    opts->release();
    if (!library_) {
        std::fprintf(stderr, "[BloomEffect] MSL compile failed: %s\n",
                     err ? err->localizedDescription()->utf8String() : "?");
        return false;
    }

    auto fmt = static_cast<MTL::PixelFormat>(colorPixelFormat);
    psoThreshold_  = buildPipeline(device, library_, "fs_threshold",  fmt, false);
    psoDownsample_ = buildPipeline(device, library_, "fs_downsample", fmt, false);
    psoUpsample_   = buildPipeline(device, library_, "fs_upsample",   fmt, true);
    psoComposite_  = buildPipeline(device, library_, "fs_composite",  fmt, false);
    if (!psoThreshold_ || !psoDownsample_ || !psoUpsample_ || !psoComposite_) {
        resetAll();
        return false;
    }

    MTL::SamplerDescriptor* sd = MTL::SamplerDescriptor::alloc()->init();
    sd->setMinFilter(MTL::SamplerMinMagFilterLinear);
    sd->setMagFilter(MTL::SamplerMinMagFilterLinear);
    sd->setMipFilter(MTL::SamplerMipFilterNotMipmapped);
    sd->setSAddressMode(MTL::SamplerAddressModeClampToEdge);
    sd->setTAddressMode(MTL::SamplerAddressModeClampToEdge);
    sampler_ = device->newSamplerState(sd);
    sd->release();

    return sampler_ != nullptr;
}

bool BloomEffect::ensureMipChain(int colorTargetWidthPx,
                                  int colorTargetHeightPx,
                                  int colorPixelFormat)
{
    if (!device_) return false;
    if (colorTargetWidthPx == cachedColorW_
        && colorTargetHeightPx == cachedColorH_
        && mips_[0] != nullptr
        && cachedPixelFormat_ == colorPixelFormat)
    {
        return true;
    }
    // Drop the old chain.
    for (int i = 0; i < kMaxMips; ++i) {
        if (mips_[i]) { mips_[i]->release(); mips_[i] = nullptr; }
        mipW_[i] = mipH_[i] = 0;
    }
    cachedColorW_      = colorTargetWidthPx;
    cachedColorH_      = colorTargetHeightPx;
    cachedPixelFormat_ = colorPixelFormat;
    auto fmt           = static_cast<MTL::PixelFormat>(colorPixelFormat);

    int w = std::max(1, colorTargetWidthPx  / 2);
    int h = std::max(1, colorTargetHeightPx / 2);
    for (int i = 0; i < kMaxMips; ++i) {
        if (w < 4 || h < 4) break;       // stop before we get smaller than a tent kernel
        mips_[i] = allocSampleableTarget(device_, w, h, fmt);
        if (!mips_[i]) return false;
        mipW_[i] = w; mipH_[i] = h;
        w = std::max(1, w / 2);
        h = std::max(1, h / 2);
    }
    return true;
}

bool BloomEffect::ensureCompositeScratch(int widthPx, int heightPx,
                                          int colorPixelFormat)
{
    if (compositeScratch_
        && compositeW_ == widthPx
        && compositeH_ == heightPx
        && cachedPixelFormat_ == colorPixelFormat) {
        return true;
    }
    if (compositeScratch_) { compositeScratch_->release(); compositeScratch_ = nullptr; }
    compositeScratch_ = allocSampleableTarget(device_, widthPx, heightPx,
        static_cast<MTL::PixelFormat>(colorPixelFormat));
    if (!compositeScratch_) return false;
    compositeW_ = widthPx;
    compositeH_ = heightPx;
    return true;
}

// ----------------------------------------------------------------------
// Per-frame helpers
// ----------------------------------------------------------------------

namespace {
// Begin a render pass writing into `dst` with LoadAction = Load (preserves
// what the previous pass wrote, important for the additive upsample) or
// DontCare (used by passes that fully cover the destination).
MTL::RenderCommandEncoder* beginPass(MTL::CommandBuffer* cb,
                                      MTL::Texture* dst,
                                      bool loadPrevious)
{
    if (!cb || !dst) return nullptr;
    MTL::RenderPassDescriptor* rpd =
        MTL::RenderPassDescriptor::renderPassDescriptor();
    auto* col = rpd->colorAttachments()->object(0);
    col->setTexture(dst);
    col->setLoadAction(loadPrevious
                        ? MTL::LoadActionLoad
                        : MTL::LoadActionDontCare);
    col->setStoreAction(MTL::StoreActionStore);
    return cb->renderCommandEncoder(rpd);
}

// Set the viewport to (0,0)..(width,height) — for our fullscreen-quad
// triangle no scissor is needed.
void setViewport(MTL::RenderCommandEncoder* enc, int w, int h) {
    MTL::Viewport vp = {0.0, 0.0,
                         static_cast<double>(w), static_cast<double>(h),
                         0.0, 1.0};
    enc->setViewport(vp);
}
} // anon

void BloomEffect::encodeThreshold(MTL::CommandBuffer* cb,
                                   MTL::Texture* src,
                                   MTL::Texture* dst,
                                   int srcW, int srcH)
{
    if (!psoThreshold_) return;
    MTL::RenderCommandEncoder* enc = beginPass(cb, dst, false);
    if (!enc) return;
    setViewport(enc, static_cast<int>(dst->width()),
                       static_cast<int>(dst->height()));
    enc->setRenderPipelineState(psoThreshold_);
    enc->setFragmentTexture(src, 0);
    enc->setFragmentSamplerState(sampler_, 0);
    float params[4] = {threshold, softKnee, 0.0f, 0.0f};
    enc->setFragmentBytes(params, sizeof(params), 0);
    enc->drawPrimitives(MTL::PrimitiveTypeTriangle,
                         static_cast<NS::UInteger>(0),
                         static_cast<NS::UInteger>(3));
    enc->endEncoding();
    (void)srcW; (void)srcH;
}

void BloomEffect::encodeDownsample(MTL::CommandBuffer* cb,
                                    MTL::Texture* src,
                                    MTL::Texture* dst,
                                    int srcW, int srcH,
                                    bool firstDownsample)
{
    if (!psoDownsample_) return;
    MTL::RenderCommandEncoder* enc = beginPass(cb, dst, false);
    if (!enc) return;
    setViewport(enc, static_cast<int>(dst->width()),
                       static_cast<int>(dst->height()));
    enc->setRenderPipelineState(psoDownsample_);
    enc->setFragmentTexture(src, 0);
    enc->setFragmentSamplerState(sampler_, 0);
    float params[4] = {
        1.0f / static_cast<float>(srcW),
        1.0f / static_cast<float>(srcH),
        firstDownsample ? 1.0f : 0.0f,
        0.0f
    };
    enc->setFragmentBytes(params, sizeof(params), 0);
    enc->drawPrimitives(MTL::PrimitiveTypeTriangle,
                         static_cast<NS::UInteger>(0),
                         static_cast<NS::UInteger>(3));
    enc->endEncoding();
}

void BloomEffect::encodeUpsample(MTL::CommandBuffer* cb,
                                  MTL::Texture* src,
                                  MTL::Texture* dst,
                                  int srcW, int srcH,
                                  float scatter)
{
    if (!psoUpsample_) return;
    MTL::RenderCommandEncoder* enc = beginPass(cb, dst, /*loadPrevious=*/true);
    if (!enc) return;
    setViewport(enc, static_cast<int>(dst->width()),
                       static_cast<int>(dst->height()));
    enc->setRenderPipelineState(psoUpsample_);
    enc->setFragmentTexture(src, 0);
    enc->setFragmentSamplerState(sampler_, 0);
    float params[4] = {
        1.0f / static_cast<float>(srcW),
        1.0f / static_cast<float>(srcH),
        scatter,
        0.0f
    };
    enc->setFragmentBytes(params, sizeof(params), 0);
    enc->drawPrimitives(MTL::PrimitiveTypeTriangle,
                         static_cast<NS::UInteger>(0),
                         static_cast<NS::UInteger>(3));
    enc->endEncoding();
}

void BloomEffect::encodeComposite(MTL::CommandBuffer* cb,
                                   MTL::Texture* scene,
                                   MTL::Texture* bloom,
                                   MTL::Texture* dst,
                                   int dstW, int dstH)
{
    if (!psoComposite_) return;
    MTL::RenderCommandEncoder* enc = beginPass(cb, dst, false);
    if (!enc) return;
    setViewport(enc, dstW, dstH);
    enc->setRenderPipelineState(psoComposite_);
    enc->setFragmentTexture(scene, 0);
    enc->setFragmentTexture(bloom, 1);
    enc->setFragmentSamplerState(sampler_, 0);
    float params[4] = {tint.x, tint.y, tint.z, intensity};
    enc->setFragmentBytes(params, sizeof(params), 0);
    enc->drawPrimitives(MTL::PrimitiveTypeTriangle,
                         static_cast<NS::UInteger>(0),
                         static_cast<NS::UInteger>(3));
    enc->endEncoding();
}

// ----------------------------------------------------------------------
// render — orchestrates the full dispatch.
// ----------------------------------------------------------------------

void BloomEffect::render(RenderContext& ctx) {
    if (state != LayerState::Enabled) return;
    if (opacity <= 0.0f)               return;
    if (!ctx.cmdBuf || !ctx.colorTarget) return;

    MTL::Device* device = ctx.colorTarget->device();
    if (!device) return;
    const int colorFmt = static_cast<int>(ctx.colorTarget->pixelFormat());
    if (!buildAll(device, colorFmt)) return;

    const int W = static_cast<int>(ctx.colorTarget->width());
    const int H = static_cast<int>(ctx.colorTarget->height());
    if (W < 8 || H < 8) return;

    if (!ensureMipChain(W, H, colorFmt))           return;
    if (!ensureCompositeScratch(W, H, colorFmt))   return;

    // How many mips to actually use this frame (clamp to allocated).
    int wantMips = std::clamp(radius, 4, kMaxMips);
    int activeMips = 0;
    for (int i = 0; i < wantMips; ++i) {
        if (!mips_[i]) break;
        ++activeMips;
    }
    if (activeMips < 2) return;   // not enough chain to do anything useful

    // ---- Pass 1: threshold (colorTarget → mip[0]) -------------------------
    // mip[0] is the bright-pass surface — every subsequent pass derives from
    // it. Source is the full-resolution colorTarget; destination is half-res
    // so the threshold also serves as the first 2× box downsample.
    encodeThreshold(ctx.cmdBuf, ctx.colorTarget, mips_[0], W, H);

    // ---- Pass 2: downsample chain (mip[0] → mip[1] → ... → mip[N-1]) -----
    // 13-tap partial-Karis-average. Karis weighting is enabled on the FIRST
    // downsample only (mip[0] → mip[1]) — that's where lone-pixel fireflies
    // live; subsequent levels are already averaged enough.
    for (int i = 0; i < activeMips - 1; ++i) {
        encodeDownsample(ctx.cmdBuf,
                          mips_[i], mips_[i + 1],
                          mipW_[i], mipH_[i],
                          /*firstDownsample=*/(i == 0));
    }

    // ---- Pass 3: upsample chain (mip[N-1] → mip[N-2] → ... → mip[0]) -----
    // 9-tap tent filter, additive blend into the destination (whatever the
    // downsample wrote earlier stays — that's the lower-frequency content;
    // the upsample contributes the higher-frequency band from the level
    // above).
    //
    // `scatter` = Jimenez's filter radius. Operator's `radius` knob feeds
    // mip count (controls reach), but `scatter` is a sub-mip widening that
    // helps the halo look softer without paying for an extra mip. We use a
    // fixed 1.0 here; tweak in the inspector if you want a fatter halo.
    constexpr float kScatter = 1.0f;
    for (int i = activeMips - 1; i > 0; --i) {
        encodeUpsample(ctx.cmdBuf,
                        mips_[i], mips_[i - 1],
                        mipW_[i], mipH_[i],
                        kScatter);
    }

    // ---- Pass 4: composite (colorTarget + mip[0]) → compositeScratch -----
    // We can't sample colorTarget while writing to it in the same encoder,
    // so we render the final mix into compositeScratch and then blit it
    // back over colorTarget.
    encodeComposite(ctx.cmdBuf,
                     ctx.colorTarget,    // scene
                     mips_[0],           // upsampled bloom
                     compositeScratch_,
                     W, H);

    // ---- Pass 5: blit compositeScratch → colorTarget ---------------------
    // No fragment work — just a GPU-side copy. Cheaper than a fullscreen
    // pass and keeps colorTarget bit-exact equal to what the composite wrote.
    MTL::BlitCommandEncoder* blit = ctx.cmdBuf->blitCommandEncoder();
    if (blit) {
        MTL::Origin origin = MTL::Origin::Make(0, 0, 0);
        MTL::Size   size   = MTL::Size::Make(W, H, 1);
        blit->copyFromTexture(compositeScratch_, 0, 0, origin, size,
                               ctx.colorTarget, 0, 0, origin);
        blit->endEncoding();
    }
}

// ----------------------------------------------------------------------
// Inspector
// ----------------------------------------------------------------------

void BloomEffect::drawInspector() {
    if (ImGui::CollapsingHeader("Threshold",
                                 ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SliderFloat("Threshold##b", &threshold, 0.0f, 2.0f, "%.2f");
        ImGui::SliderFloat("Soft knee##b", &softKnee,  0.0f, 1.0f, "%.2f");
        ImGui::TextDisabled("Luma cutoff for the bright pass.");
        ImGui::TextDisabled("Knee = 0 → hard cutoff (pops); knee = 1 → wide fade.");
    }
    if (ImGui::CollapsingHeader("Bloom",
                                 ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SliderFloat("Intensity##b", &intensity, 0.0f, 2.0f, "%.2f");
        ImGui::SliderInt  ("Radius##b",    &radius,    4,    kMaxMips,
                            "%d mips");
        ImGui::ColorEdit3 ("Tint##b",      &tint[0],
                            ImGuiColorEditFlags_PickerHueWheel
                            | ImGuiColorEditFlags_Float);
        ImGui::TextDisabled("Intensity > 1 over-boosts (saturated-sensor look).");
        ImGui::TextDisabled("Tint = (1, 0.85, 0.55) → gold rim;");
        ImGui::TextDisabled("       (0.6, 0.8, 1.0) → icy halation.");
    }
    if (ImGui::CollapsingHeader("Algorithm")) {
        ImGui::TextDisabled("Karis 2013 soft-knee bright pass");
        ImGui::TextDisabled("Jimenez 2014 13-tap downsample (CoD: AW)");
        ImGui::TextDisabled("Jimenez 2014 9-tap tent upsample (additive)");
        ImGui::TextDisabled("Karis-average firefly filter on first downsample");
        ImGui::Separator();
        ImGui::Text("Mip chain: up to %d levels", kMaxMips);
        ImGui::Text("Active this frame: %d", std::clamp(radius, 4, kMaxMips));
        if (cachedColorW_ > 0 && cachedColorH_ > 0) {
            ImGui::Text("Target: %d × %d", cachedColorW_, cachedColorH_);
            for (int i = 0; i < kMaxMips && mips_[i]; ++i) {
                ImGui::Text("  mip[%d] %d × %d", i, mipW_[i], mipH_[i]);
            }
        }
    }
}

} // namespace spacegen
