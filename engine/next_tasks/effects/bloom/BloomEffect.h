#pragma once
// BloomEffect — Karis / Jimenez physical bloom (SIGGRAPH 2014).
//
// LayerKind::Effect post-process. Reads ctx.colorTarget, builds a mip
// pyramid of the bright pass with the 13-tap downsample (Jimenez 2014
// slide 153), upsamples additively with the 9-tap tent (slide 162), and
// composites the full-resolution result back onto the scene with
// `mix(scene, bloom * tint, intensity)`.
//
// Karis (2013) soft-knee threshold is used to avoid the visible "pop" at
// the luminance cutoff, and Karis-average firefly suppression is applied
// to the first downsample only.
//
// Lifecycle:
//   - First render() lazily compiles the MSL library + builds the 4
//     pipelines (threshold / downsample / upsample / composite) and the
//     sampler / fullscreen quad.
//   - Each frame the mip chain is (re)allocated to match the current
//     colorTarget size and the operator's radius setting.
//   - Destructor releases every Metal object the effect owns.
//
// All Metal objects are owned by this layer (no MetalRenderer changes
// required at the API level — see INTEGRATION.md for optional fast paths).

#include "../../../core/Layer.h"

#include <glm/glm.hpp>

namespace MTL {
    class Device;
    class Library;
    class RenderPipelineState;
    class SamplerState;
    class Texture;
}

namespace spacegen {

class BloomEffect : public ILayer {
public:
    BloomEffect();
    ~BloomEffect() override;

    BloomEffect(const BloomEffect&)            = delete;
    BloomEffect& operator=(const BloomEffect&) = delete;

    // ---- ILayer ----
    LayerKind   kind()      const override { return LayerKind::Effect; }
    const char* typeName()  const override { return "Bloom (Karis/Jimenez)"; }
    void        render(RenderContext& ctx) override;
    void        drawInspector() override;

    // ---- Inspector parameters ----------------------------------------------
    // Luminance threshold for the bright pass. Pixels above this value
    // contribute to bloom; pixels below pass through unchanged. Operator-
    // tuned in [0, 2]. Defaults to 1.0 (the canonical "anything above
    // pure white blooms").
    float     threshold = 1.0f;

    // Soft-knee width around the threshold (Karis 2013). A non-zero knee
    // creates a quadratic fade-in in `[threshold - knee, threshold + knee]`
    // so the bright-pass transitions smoothly. `knee = 0` recovers the
    // hard threshold. Operator-tuned in [0, 1].
    float     softKnee = 0.5f;

    // Final-composite mix amount: `final = mix(scene, bloom * tint, intensity)`.
    // Operator-tuned in [0, 2] — values > 1 over-boost the bloom for the
    // look of a saturated sensor. Defaults to 0.6 (subtle but visible).
    float     intensity = 0.6f;

    // Tint multiplied with the bloom before compositing. Defaults to white.
    // Examples:
    //   (1.0, 0.85, 0.55) — classic "gold rim" warm bloom
    //   (0.6, 0.8, 1.0)   — icy halation
    //   (1.0, 0.4, 0.4)   — neon-sign red glow
    glm::vec3 tint = glm::vec3(1.0f);

    // Mip-chain depth (= halo width / softness). 4 = tight, sharp bloom;
    // 7 = wide, ethereal halo. Operator-tuned in [4, 7]. Clamped on use.
    int       radius = 6;

private:
    // Lazy build / teardown of all Metal objects the effect owns. Idempotent:
    // safe to call buildAll() multiple times — bails out if everything is
    // already built. resetAll() releases everything (used by the destructor
    // and by buildAll on a device mismatch).
    bool buildAll(MTL::Device* device, int colorPixelFormat);
    void resetAll();

    // (Re)allocate the mip chain to (widthPx, heightPx) at the given
    // pixel format if any of those changed since last frame. The mip chain
    // is sized as: mip[0] = ½ of colorTarget; mip[i+1] = ½ of mip[i].
    bool ensureMipChain(int colorTargetWidthPx,
                        int colorTargetHeightPx,
                        int colorPixelFormat);

    // (Re)allocate the composite-scratch texture (full-res, same format).
    bool ensureCompositeScratch(int widthPx,
                                int heightPx,
                                int colorPixelFormat);

    // Per-frame helpers (run on ctx.cmdBuf, never own the encoder past
    // the call returning). All fail silently if any required object is
    // null — we never crash the workstation because of an effect.
    void encodeThreshold(class MTL::CommandBuffer* cb,
                         MTL::Texture* src,
                         MTL::Texture* dst,
                         int srcW, int srcH);
    void encodeDownsample(class MTL::CommandBuffer* cb,
                          MTL::Texture* src,
                          MTL::Texture* dst,
                          int srcW, int srcH,
                          bool firstDownsample);
    void encodeUpsample(class MTL::CommandBuffer* cb,
                        MTL::Texture* src,
                        MTL::Texture* dst,
                        int srcW, int srcH,
                        float scatter);
    void encodeComposite(class MTL::CommandBuffer* cb,
                         MTL::Texture* scene,
                         MTL::Texture* bloom,
                         MTL::Texture* dst,
                         int dstW, int dstH);

    // ---- Owned Metal objects ----
    MTL::Device*               device_     = nullptr; // non-owning
    int                        cachedPixelFormat_ = 0;
    MTL::Library*              library_    = nullptr;
    MTL::SamplerState*         sampler_    = nullptr; // bilinear clamp-to-edge

    MTL::RenderPipelineState*  psoThreshold_   = nullptr;
    MTL::RenderPipelineState*  psoDownsample_  = nullptr;
    MTL::RenderPipelineState*  psoUpsample_    = nullptr;  // additive blend
    MTL::RenderPipelineState*  psoComposite_   = nullptr;

    // Bright-pass + downsample chain. mip[0] = ½ resolution, mip[i+1] = ½
    // resolution of mip[i]. Allocated up to kMaxMips levels; the effective
    // number used per-frame is `clamp(radius, 4, 7)`.
    static constexpr int       kMaxMips = 7;
    MTL::Texture*              mips_[kMaxMips]   = {nullptr};
    int                        mipW_[kMaxMips]   = {0};
    int                        mipH_[kMaxMips]   = {0};
    int                        cachedColorW_     = 0;
    int                        cachedColorH_     = 0;

    // Composite scratch: full-resolution surface the final pass writes into.
    // We can't sample colorTarget and write to it in the same encoder, so
    // we composite into this then blit back.
    MTL::Texture*              compositeScratch_ = nullptr;
    int                        compositeW_       = 0;
    int                        compositeH_       = 0;
};

} // namespace spacegen
