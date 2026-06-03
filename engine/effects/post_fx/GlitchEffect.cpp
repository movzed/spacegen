#include "GlitchEffect.h"

#include "../../../backends/metal/MetalRenderer.h"
#include "../../../core/Scene.h"
#include "../../../core/ModulatorBank.h"

#include "imgui.h"

#include <Metal/Metal.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace spacegen {

namespace {

// MSL source, pulled from glitch.metal.inc as a raw-string literal.
constexpr const char* kGlitchMsl =
#include "glitch.metal.inc"
    ;

// MSL-side layout — must match the struct in glitch.metal.inc exactly.
struct GlitchUniforms {
    glm::vec4 amountBeat;   // .x amount .y beatPhase .z beatIndex .w time
    glm::vec4 paramsA;      // .x seed   .y bandPx    .z 1/W       .w 1/H
    glm::vec4 enables;      // .x bands  .y swap      .z dropout   .w tear
    glm::vec4 thresholds;   // .x bands  .y swap      .z dropout   .w tear
    glm::vec4 flashCtrl;    // .x enable .y threshold
    glm::vec4 flashColor;   // .rgb color
};

} // namespace

GlitchEffect::GlitchEffect() {
    name      = "Glitch";
    blendMode = BlendMode::Normal;
    colorTag  = glm::vec3(0.95f, 0.85f, 0.20f);
    opacity   = 1.0f;

    // Per-sub-effect activation thresholds tuned for a satisfying ramp:
    // at amount 0.1 you see bands; 0.3 adds RGB swap; 0.6 the screen
    // falls apart. See README.md "Why thresholds and not weights?"
    bands.threshold   = 0.05f;
    rgbSwap.threshold = 0.20f;
    dropout.threshold = 0.30f;
    tear.threshold    = 0.45f;
    tear.enabled      = false;   // off by default — tear is loud
    flash.threshold   = 0.60f;
    flash.enabled     = false;   // off by default — flash is louder
}

GlitchEffect::~GlitchEffect() {
    if (pipeline_) pipeline_->release();
}

void GlitchEffect::buildPipeline(MTL::Device* device, MTL::PixelFormat fmt) {
    if (pipeline_ && fmt == builtFmt_) return;
    if (pipeline_) {
        pipeline_->release();
        pipeline_ = nullptr;
    }

    NS::Error* err = nullptr;
    NS::String* src = NS::String::string(kGlitchMsl, NS::UTF8StringEncoding);
    MTL::CompileOptions* opts = MTL::CompileOptions::alloc()->init();
    MTL::Library* lib = device->newLibrary(src, opts, &err);
    opts->release();
    if (!lib) {
        std::fprintf(stderr, "[Glitch] MSL compile failed: %s\n",
                      err ? err->localizedDescription()->utf8String()
                          : "(no error)");
        return;
    }

    MTL::Function* vsFn = lib->newFunction(
        NS::String::string("glitch_vs", NS::UTF8StringEncoding));
    MTL::Function* fsFn = lib->newFunction(
        NS::String::string("glitch_fs", NS::UTF8StringEncoding));
    if (!vsFn || !fsFn) {
        std::fprintf(stderr, "[Glitch] glitch_vs/glitch_fs not found\n");
        if (vsFn) vsFn->release();
        if (fsFn) fsFn->release();
        lib->release();
        return;
    }

    MTL::RenderPipelineDescriptor* pd =
        MTL::RenderPipelineDescriptor::alloc()->init();
    pd->setVertexFunction(vsFn);
    pd->setFragmentFunction(fsFn);
    pd->colorAttachments()->object(0)->setPixelFormat(fmt);

    pipeline_ = device->newRenderPipelineState(pd, &err);
    pd->release();
    vsFn->release();
    fsFn->release();
    lib->release();

    if (!pipeline_) {
        std::fprintf(stderr, "[Glitch] pipeline build failed: %s\n",
                      err ? err->localizedDescription()->utf8String()
                          : "(no error)");
        return;
    }
    builtFmt_ = fmt;
}

float GlitchEffect::computeEffectiveAmount(const RenderContext& ctx) const {
    float amount = masterAmount;

    // ModulatorBank contribution: LFO output is normalized -1..+1.
    if (ctx.scene) {
        const ModulatorBank& mods = ctx.scene->modulators;
        amount += mods.eval(amountModSlot, ctx.elapsedSeconds)
                  * amountModDepth;
    }

    // BPM-locked envelope: exp decay starting at each beat boundary.
    // bpm = 0 → no schedule (operator wants continuous mode).
    if (bpm > 1e-3f) {
        double beats     = ctx.elapsedSeconds * static_cast<double>(bpm)
                         / 60.0;
        double beatPhase = beats - std::floor(beats);   // 0..1
        float  env       = std::exp(-static_cast<float>(beatPhase)
                                    * std::max(beatEnvDecay, 0.1f));
        amount += env * beatStrength;
    }

    return std::clamp(amount, 0.0f, 1.0f);
}

void GlitchEffect::render(RenderContext& ctx) {
    if (!ctx.renderer || !ctx.cmdBuf || !ctx.colorTarget) return;

    // Same render-into-scratch + blit-back pattern as ChromaticAberration:
    // do NOT swapPingPong because the Workstation displays a fixed
    // texture pointer captured at beginFrame.
    MTL::Texture* dst = ctx.renderer->acquirePingPongTarget(ctx);
    if (!dst) return;
    MTL::Texture* srcTex = ctx.colorTarget;

    MTL::Device* device = srcTex->device();
    buildPipeline(device, srcTex->pixelFormat());
    if (!pipeline_) return;

    // ---- Resolve effective amount + BPM beat ----
    float A = computeEffectiveAmount(ctx);

    double beatIndex = 0.0;
    double beatPhase = 0.0;
    if (bpm > 1e-3f) {
        double beats = ctx.elapsedSeconds * static_cast<double>(bpm) / 60.0;
        beatIndex = std::floor(beats);
        beatPhase = beats - beatIndex;
    } else {
        // No BPM → re-roll every frame using frameIndex as the "beat".
        beatIndex = static_cast<double>(ctx.frameIndex);
        beatPhase = 0.0;
    }

    const int w = ctx.width  > 0 ? ctx.width  : 1;
    const int h = ctx.height > 0 ? ctx.height : 1;

    GlitchUniforms u{};
    u.amountBeat = glm::vec4(A,
                              static_cast<float>(beatPhase),
                              static_cast<float>(beatIndex),
                              static_cast<float>(ctx.elapsedSeconds));
    u.paramsA = glm::vec4(static_cast<float>(seed),
                           static_cast<float>(std::max(bandThicknessPx, 1)),
                           1.0f / static_cast<float>(w),
                           1.0f / static_cast<float>(h));
    u.enables = glm::vec4(bands.enabled   ? 1.0f : 0.0f,
                           rgbSwap.enabled ? 1.0f : 0.0f,
                           dropout.enabled ? 1.0f : 0.0f,
                           tear.enabled    ? 1.0f : 0.0f);
    u.thresholds = glm::vec4(bands.threshold,
                              rgbSwap.threshold,
                              dropout.threshold,
                              tear.threshold);
    u.flashCtrl  = glm::vec4(flash.enabled ? 1.0f : 0.0f,
                              flash.threshold,
                              0.0f, 0.0f);
    u.flashColor = glm::vec4(flashColor, 0.0f);

    // ---- Encode the fullscreen pass ----
    MTL::RenderPassDescriptor* rpd =
        MTL::RenderPassDescriptor::renderPassDescriptor();
    auto* ca = rpd->colorAttachments()->object(0);
    ca->setTexture(dst);
    ca->setLoadAction(MTL::LoadActionDontCare);
    ca->setStoreAction(MTL::StoreActionStore);

    MTL::RenderCommandEncoder* enc = ctx.cmdBuf->renderCommandEncoder(rpd);
    if (!enc) return;

    enc->setRenderPipelineState(pipeline_);
    enc->setFragmentBytes(&u, sizeof(u), 0);
    enc->setFragmentTexture(srcTex, 0);
    enc->setFragmentSamplerState(ctx.renderer->postFxSampler(), 0);
    enc->drawPrimitives(MTL::PrimitiveTypeTriangleStrip,
                         NS::UInteger(0), NS::UInteger(4));
    enc->endEncoding();

    // Blit the scratch result back over colorTarget.
    MTL::BlitCommandEncoder* blit = ctx.cmdBuf->blitCommandEncoder();
    if (blit) {
        MTL::Origin o = MTL::Origin::Make(0, 0, 0);
        MTL::Size   s = MTL::Size::Make(ctx.colorTarget->width(),
                                         ctx.colorTarget->height(), 1);
        blit->copyFromTexture(dst, 0, 0, o, s, ctx.colorTarget, 0, 0, o);
        blit->endEncoding();
    }
}

void GlitchEffect::drawInspector() {
    ImGui::SliderFloat("Master amount##gl", &masterAmount, 0.0f, 1.0f);

    ImGui::Separator();
    ImGui::TextDisabled("BPM schedule");
    ImGui::SliderFloat("BPM##gl",         &bpm,          0.0f, 240.0f,
                       bpm < 0.5f ? "off" : "%.1f");
    ImGui::SliderFloat("Beat strength##gl", &beatStrength, 0.0f, 1.0f);
    ImGui::SliderFloat("Beat decay##gl",    &beatEnvDecay, 1.0f, 32.0f);

    ImGui::Separator();
    ImGui::TextDisabled("Mod bank (amount)");
    static const char* kModNames[] = {
        "Unbound", "LFO 1", "LFO 2", "LFO 3",
        "LFO 4",   "LFO 5", "LFO 6", "LFO 7", "LFO 8"
    };
    ImGui::SetNextItemWidth(110.0f);
    ImGui::Combo("##glm", &amountModSlot, kModNames, 9);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140.0f);
    ImGui::SliderFloat("Depth##glm", &amountModDepth, 0.0f, 1.0f);

    ImGui::Separator();
    {
        int s = static_cast<int>(seed);
        if (ImGui::SliderInt("Seed##gl", &s, 0, 65535)) {
            seed = static_cast<uint32_t>(s);
        }
    }
    ImGui::SliderInt("Band thickness (px)##gl",
                     &bandThicknessPx, 2, 64);

    // ---- Sub-effects ---------------------------------------------------
    auto subRow = [&](const char* label, SubEffect& fx, const char* idTag) {
        ImGui::PushID(idTag);
        ImGui::Checkbox("##en", &fx.enabled);
        ImGui::SameLine();
        ImGui::TextUnformatted(label);
        ImGui::SameLine(160.0f);
        ImGui::SetNextItemWidth(140.0f);
        ImGui::SliderFloat("threshold", &fx.threshold, 0.0f, 1.0f);
        ImGui::PopID();
    };

    ImGui::Separator();
    ImGui::TextDisabled("Sub-effects (fire when amount > threshold)");
    subRow("Displacement bands", bands,   "fxb");
    subRow("RGB swap",           rgbSwap, "fxs");
    subRow("Block dropout",      dropout, "fxd");
    subRow("Tear line",          tear,    "fxt");
    subRow("Color flash",        flash,   "fxf");

    if (flash.enabled) {
        ImGui::ColorEdit3("Flash color##gl", &flashColor[0],
                           ImGuiColorEditFlags_Float
                           | ImGuiColorEditFlags_PickerHueWheel);
    }

    ImGui::Separator();
    ImGui::TextDisabled("Glitches snap to the BPM grid — set BPM to the");
    ImGui::TextDisabled("track tempo so corruption hits on the downbeat.");
    ImGui::TextDisabled("Set BPM=0 for continuous re-roll every frame.");
}

} // namespace spacegen
