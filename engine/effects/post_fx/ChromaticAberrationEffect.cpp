#include "ChromaticAberrationEffect.h"

#include "../../../backends/metal/MetalRenderer.h"
#include "../../../core/Scene.h"
#include "../../../core/ModulatorBank.h"

#include "imgui.h"

#include <Metal/Metal.hpp>
#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace spacegen {

namespace {

// MSL source, pulled from ca.metal.inc as a raw-string literal.
constexpr const char* kCaMsl =
#include "ca.metal.inc"
    ;

struct CaUniforms {
    glm::vec4 params; // .x strength, .yz center, .w pattern
};

} // namespace

ChromaticAberrationEffect::ChromaticAberrationEffect() {
    name      = "Chromatic Aberration";
    blendMode = BlendMode::Normal;
    colorTag  = glm::vec3(0.95f, 0.30f, 0.55f);
    opacity   = 1.0f;
}

ChromaticAberrationEffect::~ChromaticAberrationEffect() {
    if (pipeline_) pipeline_->release();
}

void ChromaticAberrationEffect::buildPipeline(MTL::Device* device,
                                                MTL::PixelFormat fmt)
{
    if (pipeline_ && fmt == builtFmt_) return;
    if (pipeline_) {
        pipeline_->release();
        pipeline_ = nullptr;
    }

    NS::Error* err = nullptr;
    NS::String* src = NS::String::string(kCaMsl, NS::UTF8StringEncoding);
    MTL::CompileOptions* opts = MTL::CompileOptions::alloc()->init();
    MTL::Library* lib = device->newLibrary(src, opts, &err);
    opts->release();
    if (!lib) {
        std::string msg = "[CA] MSL compile failed: ";
        if (err) msg += err->localizedDescription()->utf8String();
        throw std::runtime_error(msg);
    }

    MTL::Function* vsFn = lib->newFunction(
        NS::String::string("ca_vs", NS::UTF8StringEncoding));
    MTL::Function* fsFn = lib->newFunction(
        NS::String::string("ca_fs", NS::UTF8StringEncoding));
    if (!vsFn || !fsFn) {
        if (vsFn) vsFn->release();
        if (fsFn) fsFn->release();
        lib->release();
        throw std::runtime_error("[CA] ca_vs/ca_fs not found in library");
    }

    MTL::RenderPipelineDescriptor* pd =
        MTL::RenderPipelineDescriptor::alloc()->init();
    pd->setVertexFunction(vsFn);
    pd->setFragmentFunction(fsFn);
    // No vertex descriptor — we use vertex_id-based corner table in VS.
    pd->colorAttachments()->object(0)->setPixelFormat(fmt);
    // No depth attachment (post-process pass).

    pipeline_ = device->newRenderPipelineState(pd, &err);
    pd->release();
    vsFn->release();
    fsFn->release();
    lib->release();

    if (!pipeline_) {
        std::string msg = "[CA] pipeline build failed: ";
        if (err) msg += err->localizedDescription()->utf8String();
        throw std::runtime_error(msg);
    }
    builtFmt_ = fmt;
}

void ChromaticAberrationEffect::render(RenderContext& ctx) {
    if (!ctx.renderer || !ctx.cmdBuf || !ctx.colorTarget) return;

    // Acquire ping-pong target from the renderer. The renderer owns this
    // texture; we render into it then ask the renderer to swap so the
    // next layer / present sees the result.
    MTL::Texture* dst = ctx.renderer->acquirePingPongTarget(ctx);
    if (!dst) return;   // renderer couldn't allocate — bail silently
    MTL::Texture* src = ctx.colorTarget;

    // Lazy pipeline build (first frame or format change).
    MTL::Device* device = src->device();
    buildPipeline(device, src->pixelFormat());

    // Resolve effective strength: static slider + (optional) mod-bank.
    float effStrength = strength;
    if (ctx.scene) {
        const ModulatorBank& mods = ctx.scene->modulators;
        effStrength += mods.eval(strengthModSlot, ctx.elapsedSeconds)
                       * strengthModDepth;
    }
    effStrength = std::clamp(effStrength, 0.0f, 0.5f);

    CaUniforms u{};
    u.params = glm::vec4(effStrength,
                          std::clamp(center.x, 0.0f, 1.0f),
                          std::clamp(center.y, 0.0f, 1.0f),
                          static_cast<float>(pattern));

    MTL::RenderPassDescriptor* rpd =
        MTL::RenderPassDescriptor::renderPassDescriptor();
    auto* ca = rpd->colorAttachments()->object(0);
    ca->setTexture(dst);
    ca->setLoadAction(MTL::LoadActionDontCare);   // full-screen, overwrites all
    ca->setStoreAction(MTL::StoreActionStore);

    MTL::RenderCommandEncoder* enc = ctx.cmdBuf->renderCommandEncoder(rpd);
    if (!enc) return;

    enc->setRenderPipelineState(pipeline_);
    enc->setFragmentBytes(&u, sizeof(u), 0);
    enc->setFragmentTexture(src, 0);
    enc->setFragmentSamplerState(ctx.renderer->postFxSampler(), 0);
    enc->drawPrimitives(MTL::PrimitiveTypeTriangleStrip,
                         NS::UInteger(0), NS::UInteger(4));
    enc->endEncoding();

    // Swap so subsequent layers read our output as the "current" color.
    ctx.renderer->swapPingPong(ctx);
}

void ChromaticAberrationEffect::drawInspector() {
    ImGui::SliderFloat("Strength##ca",  &strength, 0.0f, 0.1f, "%.4f");
    ImGui::SliderFloat("Center X##ca",  &center.x, 0.0f, 1.0f);
    ImGui::SliderFloat("Center Y##ca",  &center.y, 0.0f, 1.0f);

    static const char* kPatternNames[] = {
        "Radial", "Linear", "Barrel", "Pincushion"
    };
    int p = static_cast<int>(pattern);
    if (ImGui::Combo("Pattern##ca", &p, kPatternNames, 4)) {
        pattern = static_cast<Pattern>(p);
    }

    ImGui::Separator();
    ImGui::TextDisabled("Modulator bank binding (strength)");
    static const char* kModNames[] = {
        "Unbound", "LFO 1", "LFO 2", "LFO 3",
        "LFO 4",   "LFO 5", "LFO 6", "LFO 7", "LFO 8"
    };
    ImGui::SetNextItemWidth(110.0f);
    ImGui::Combo("##cam", &strengthModSlot, kModNames, 9);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140.0f);
    ImGui::SliderFloat("Depth##cam", &strengthModDepth, 0.0f, 0.1f);

    ImGui::Separator();
    ImGui::TextDisabled("Red outside, blue inside (Cauchy 1836).");
    ImGui::TextDisabled("3 samples / fragment. Cost is constant in screen res.");
}

} // namespace spacegen
