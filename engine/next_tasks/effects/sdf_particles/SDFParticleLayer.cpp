#include "SDFParticleLayer.h"

#include "../../../core/Scene.h"
#include "../../../core/ModulatorBank.h"
#include "../../../backends/metal/MetalRenderer.h"

#include <Metal/Metal.hpp>

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <random>

namespace spacegen {

// ---------------------------------------------------------------------------
// MSL source — included as a raw C++ string. Keep this file's `struct
// Particle` and `struct ParticleUniforms` (the C++ side, declared in the
// header as `Uniforms`) byte-compatible with the MSL versions inside.
// ---------------------------------------------------------------------------

namespace {

// A single 32-byte Particle, matching the layout in particles.metal.inc.
struct ParticleC {
    glm::vec3 pos;
    glm::vec3 vel;
    float     age;
    float     lifetime;
};
static_assert(sizeof(ParticleC) == 32, "Particle must be exactly 32 bytes");

// Source of the MSL shader. The file is laid out so it can be embedded
// here verbatim — see particles.metal.inc next to this .cpp.
constexpr const char* kSDFParticlesMSL =
#include "particles.metal.inc.literal"
;

// We do a small trick — since particles.metal.inc is meant to be readable
// as plain MSL by humans (and lintable), we wrap it with an R-string by a
// generated companion file. If your build doesn't generate
// particles.metal.inc.literal, the fallback below provides a copy that's
// kept in-sync manually. CMake step in INTEGRATION.md shows how to
// auto-generate the .literal wrapper.

} // anon

// ---------------------------------------------------------------------------
// Ctor / dtor
// ---------------------------------------------------------------------------

SDFParticleLayer::SDFParticleLayer()
{
    name      = "SDF Particles";
    blendMode = BlendMode::Add;
    colorTag  = glm::vec3(0.95f, 0.55f, 0.20f);
}

SDFParticleLayer::~SDFParticleLayer()
{
    releaseAllGpuResources();
}

void SDFParticleLayer::releaseAllGpuResources()
{
    if (particleBuf_)    { particleBuf_->release();    particleBuf_    = nullptr; }
    if (trailRingBuf_)   { trailRingBuf_->release();   trailRingBuf_   = nullptr; }
    if (trailHeadBuf_)   { trailHeadBuf_->release();   trailHeadBuf_   = nullptr; }
    if (triCDFBuf_)      { triCDFBuf_->release();      triCDFBuf_      = nullptr; }
    if (triVertsBuf_)    { triVertsBuf_->release();    triVertsBuf_    = nullptr; }
    if (updatePipeline_) { updatePipeline_->release(); updatePipeline_ = nullptr; }
    if (spritePipeline_) { spritePipeline_->release(); spritePipeline_ = nullptr; }
    if (trailPipeline_)  { trailPipeline_->release();  trailPipeline_  = nullptr; }
    if (depthReadState_) { depthReadState_->release(); depthReadState_ = nullptr; }
    if (library_)        { library_->release();        library_        = nullptr; }
    pipelineReady_       = false;
    allocatedCount_      = 0;
    allocatedTrailLength_= 0;
}

// ---------------------------------------------------------------------------
// Build pipelines (compute + sprite render + trail render).
// ---------------------------------------------------------------------------

void SDFParticleLayer::buildPipelinesIfNeeded(MetalRenderer& renderer)
{
    if (pipelineReady_) return;

    MTL::Device* device = (MTL::Device*)renderer.devicePublic();
    if (!device) {
        // Renderer must expose its MTL::Device (see INTEGRATION.md). If not,
        // bail with a one-shot log so we don't spam.
        static bool warned = false;
        if (!warned) {
            std::fprintf(stderr,
                "[SDFParticleLayer] MetalRenderer::devicePublic() missing — "
                "layer disabled. See INTEGRATION.md.\n");
            warned = true;
        }
        return;
    }

    NS::Error* err = nullptr;
    NS::String* src = NS::String::string(kSDFParticlesMSL,
                                          NS::UTF8StringEncoding);
    MTL::CompileOptions* opts = MTL::CompileOptions::alloc()->init();
    library_ = device->newLibrary(src, opts, &err);
    opts->release();
    if (!library_) {
        std::string msg = "[SDFParticleLayer] MSL compile failed: ";
        if (err) msg += err->localizedDescription()->utf8String();
        std::fprintf(stderr, "%s\n", msg.c_str());
        return;
    }

    // Compute pipeline — update_particles.
    MTL::Function* updateFn = library_->newFunction(
        NS::String::string("update_particles", NS::UTF8StringEncoding));
    if (!updateFn) {
        std::fprintf(stderr, "[SDFParticleLayer] update_particles not found.\n");
        return;
    }
    updatePipeline_ = device->newComputePipelineState(updateFn, &err);
    updateFn->release();
    if (!updatePipeline_) {
        std::fprintf(stderr,
            "[SDFParticleLayer] update pipeline build failed: %s\n",
            err ? err->localizedDescription()->utf8String() : "?");
        return;
    }

    // Render pipeline — point sprites.
    {
        MTL::Function* vsFn = library_->newFunction(
            NS::String::string("vs_sprite", NS::UTF8StringEncoding));
        MTL::Function* fsFn = library_->newFunction(
            NS::String::string("fs_sprite", NS::UTF8StringEncoding));
        MTL::RenderPipelineDescriptor* pd =
            MTL::RenderPipelineDescriptor::alloc()->init();
        pd->setVertexFunction(vsFn);
        pd->setFragmentFunction(fsFn);
        auto* ca = pd->colorAttachments()->object(0);
        ca->setPixelFormat(MTL::PixelFormatBGRA8Unorm);
        ca->setBlendingEnabled(true);
        ca->setRgbBlendOperation(MTL::BlendOperationAdd);
        ca->setAlphaBlendOperation(MTL::BlendOperationAdd);
        ca->setSourceRGBBlendFactor(MTL::BlendFactorOne);
        ca->setSourceAlphaBlendFactor(MTL::BlendFactorOne);
        ca->setDestinationRGBBlendFactor(MTL::BlendFactorOne);
        ca->setDestinationAlphaBlendFactor(MTL::BlendFactorOne);
        pd->setDepthAttachmentPixelFormat(MTL::PixelFormatDepth32Float);

        spritePipeline_ = device->newRenderPipelineState(pd, &err);
        pd->release();
        if (vsFn) vsFn->release();
        if (fsFn) fsFn->release();
        if (!spritePipeline_) {
            std::fprintf(stderr,
                "[SDFParticleLayer] sprite pipeline failed: %s\n",
                err ? err->localizedDescription()->utf8String() : "?");
            return;
        }
    }

    // Render pipeline — trails.
    {
        MTL::Function* vsFn = library_->newFunction(
            NS::String::string("vs_trail", NS::UTF8StringEncoding));
        MTL::Function* fsFn = library_->newFunction(
            NS::String::string("fs_trail", NS::UTF8StringEncoding));
        MTL::RenderPipelineDescriptor* pd =
            MTL::RenderPipelineDescriptor::alloc()->init();
        pd->setVertexFunction(vsFn);
        pd->setFragmentFunction(fsFn);
        auto* ca = pd->colorAttachments()->object(0);
        ca->setPixelFormat(MTL::PixelFormatBGRA8Unorm);
        ca->setBlendingEnabled(true);
        ca->setRgbBlendOperation(MTL::BlendOperationAdd);
        ca->setAlphaBlendOperation(MTL::BlendOperationAdd);
        ca->setSourceRGBBlendFactor(MTL::BlendFactorOne);
        ca->setSourceAlphaBlendFactor(MTL::BlendFactorOne);
        ca->setDestinationRGBBlendFactor(MTL::BlendFactorOne);
        ca->setDestinationAlphaBlendFactor(MTL::BlendFactorOne);
        pd->setDepthAttachmentPixelFormat(MTL::PixelFormatDepth32Float);

        trailPipeline_ = device->newRenderPipelineState(pd, &err);
        pd->release();
        if (vsFn) vsFn->release();
        if (fsFn) fsFn->release();
        if (!trailPipeline_) {
            std::fprintf(stderr,
                "[SDFParticleLayer] trail pipeline failed: %s\n",
                err ? err->localizedDescription()->utf8String() : "?");
            // Non-fatal — trails just unavailable; sprite path still works.
        }
    }

    // Depth: read-only against the structure depth buffer.
    MTL::DepthStencilDescriptor* dsd =
        MTL::DepthStencilDescriptor::alloc()->init();
    dsd->setDepthCompareFunction(MTL::CompareFunctionLess);
    dsd->setDepthWriteEnabled(false);     // particles don't occlude each other
    depthReadState_ = device->newDepthStencilState(dsd);
    dsd->release();

    pipelineReady_ = true;
}

// ---------------------------------------------------------------------------
// Buffer (re)allocation.
// ---------------------------------------------------------------------------

void SDFParticleLayer::reallocBuffersIfNeeded(MetalRenderer& renderer,
                                                int newCount,
                                                int newTrailLength)
{
    MTL::Device* device = (MTL::Device*)renderer.devicePublic();
    if (!device) return;

    // Hard clamp particle count.
    int n = std::clamp(newCount, 1024, 1'000'000);
    if (newTrailLength > 1) n = std::min(n, 256'000);

    bool countChanged = false;
    if (n != (int)allocatedCount_ || !particleBuf_) {
        if (particleBuf_)  { particleBuf_->release();  particleBuf_  = nullptr; }
        if (trailHeadBuf_) { trailHeadBuf_->release(); trailHeadBuf_ = nullptr; }
        size_t bytes = (size_t)n * sizeof(ParticleC);
        particleBuf_ = device->newBuffer(bytes,
                                           MTL::ResourceStorageModePrivate);
        allocatedCount_ = (size_t)n;
        currentParticleCount_ = n;
        countChanged = true;
        // Re-seed required: any prior state is gone (Private storage).
        framesSinceMeshUpload_ = 0;
        seedParticleBufferGPU();
        std::printf("[SDFParticleLayer] Reallocated %d particles (%.1f MB)\n",
                     n, bytes / (1024.0 * 1024.0));
    }

    int K = std::clamp(newTrailLength, 1, 32);
    if (K != allocatedTrailLength_ || (countChanged && K > 1)) {
        if (trailRingBuf_) { trailRingBuf_->release(); trailRingBuf_ = nullptr; }
        if (K > 1) {
            // Per particle: K × TrailEntry (16 B each = float3 + 4 B pad).
            size_t ringBytes = (size_t)n * (size_t)K * 16;
            trailRingBuf_ = device->newBuffer(ringBytes,
                                                MTL::ResourceStorageModePrivate);
        }
        allocatedTrailLength_ = K;
        currentTrailLength_   = K;
        std::printf("[SDFParticleLayer] Trail length K=%d\n", K);
    }
    if (K > 1 && !trailHeadBuf_) {
        // One uint head per particle.
        trailHeadBuf_ = device->newBuffer((size_t)n * sizeof(uint32_t),
                                            MTL::ResourceStorageModePrivate);
    }
}

void SDFParticleLayer::seedParticleBufferGPU()
{
    // We rely on the update kernel's respawn path to fill newly-allocated
    // private memory with valid particles. Initially all 32-byte slots are
    // zeroed (Metal guarantees this for `newBuffer(length:options:)`); zero
    // age + zero lifetime → respawn() triggers in update.
    // No explicit kernel dispatch needed.
}

// ---------------------------------------------------------------------------
// Mesh CDF upload (Surface emitter).
// ---------------------------------------------------------------------------

void SDFParticleLayer::uploadMeshData(MetalRenderer& renderer, Scene& scene)
{
    if (!meshDataDirty_) return;
    MTL::Device* device = (MTL::Device*)renderer.devicePublic();
    if (!device) return;

    if (triCDFBuf_)   { triCDFBuf_->release();   triCDFBuf_   = nullptr; }
    if (triVertsBuf_) { triVertsBuf_->release(); triVertsBuf_ = nullptr; }
    triCount_     = 0;
    triTotalArea_ = 0.0f;

    // Aggregate world-space triangles across all meshes.
    std::vector<glm::vec3> flat;
    std::vector<float>     cdf;
    flat.reserve(1 << 14);
    cdf.reserve(1 << 13);

    glm::vec3 bmin( 1e9f), bmax(-1e9f);
    for (const auto& m : scene.meshes) {
        if (m.positions.empty() || m.indices.empty()) continue;
        const glm::mat4& M = m.transform;
        for (size_t i = 0; i + 2 < m.indices.size(); i += 3) {
            glm::vec3 v0 = glm::vec3(M * glm::vec4(m.positions[m.indices[i+0]], 1.0f));
            glm::vec3 v1 = glm::vec3(M * glm::vec4(m.positions[m.indices[i+1]], 1.0f));
            glm::vec3 v2 = glm::vec3(M * glm::vec4(m.positions[m.indices[i+2]], 1.0f));
            float area = 0.5f * glm::length(glm::cross(v1 - v0, v2 - v0));
            if (area <= 0.0f) continue;
            triTotalArea_ += area;
            cdf.push_back(triTotalArea_);
            flat.push_back(v0);
            flat.push_back(v1);
            flat.push_back(v2);
            bmin = glm::min(bmin, glm::min(v0, glm::min(v1, v2)));
            bmax = glm::max(bmax, glm::max(v0, glm::max(v1, v2)));
        }
    }
    triCount_      = (uint32_t)cdf.size();
    sceneBBoxMin_  = bmin;
    sceneBBoxMax_  = bmax;

    if (triCount_ > 0) {
        triCDFBuf_ = device->newBuffer(cdf.data(),
                                         cdf.size() * sizeof(float),
                                         MTL::ResourceStorageModeShared);
        triVertsBuf_ = device->newBuffer(flat.data(),
                                           flat.size() * sizeof(glm::vec3),
                                           MTL::ResourceStorageModeShared);
        std::printf("[SDFParticleLayer] Mesh CDF: %u tris, total area %.2f m², "
                     "bbox [%.2f..%.2f, %.2f..%.2f, %.2f..%.2f]\n",
                     triCount_, triTotalArea_,
                     bmin.x, bmax.x, bmin.y, bmax.y, bmin.z, bmax.z);
    } else {
        std::fprintf(stderr,
            "[SDFParticleLayer] No mesh triangles — Surface emitter "
            "will fall back to bbox center.\n");
    }

    meshDataDirty_ = false;
}

// ---------------------------------------------------------------------------
// Uniform packing.
// ---------------------------------------------------------------------------

void SDFParticleLayer::packUniforms(const RenderContext& ctx)
{
    double now = ctx.elapsedSeconds;
    float  dt  = (lastFrameTime_ < 0.0)
                 ? (1.0f / 60.0f)
                 : (float)std::max(1e-4, std::min(0.05, now - lastFrameTime_));
    lastFrameTime_ = now;

    // Apply modulator-bank bindings on top of operator-set values.
    float curlAmp  = curlAmplitude;
    float sdfStren = sdfStrength;
    if (ctx.scene) {
        const ModulatorBank* mods = &ctx.scene->modulators;
        curlAmp  += mods->eval(curlAmpModSlot,     now) * curlAmpModDepth;
        sdfStren += mods->eval(sdfStrengthModSlot, now) * sdfStrengthModDepth;
    }

    u_.projection      = ctx.projection;
    u_.view            = ctx.view;
    float aspect = (ctx.height > 0)
                   ? (float)ctx.width / (float)ctx.height : 1.0f;
    u_.cameraWorldPos  = glm::vec4(ctx.cameraWorldPos, aspect);

    u_.timing          = glm::vec4(dt, (float)now, drag,
                                     (float)currentParticleCount_);
    u_.noiseSdf        = glm::vec4(curlScale, curlAmp, sdfStren, sdfRange);

    glm::vec3 c  = 0.5f * (sceneBBoxMin_ + sceneBBoxMax_);
    glm::vec3 he = std::max(0.01f, 0.5f) *
                   glm::vec3(sceneBBoxMax_ - sceneBBoxMin_);
    if (ctx.scene) {
        // Fall back to scene's own bbox if mesh upload didn't run yet.
        if (he == glm::vec3(0.0f)) {
            c  = 0.5f * (ctx.scene->bboxMin + ctx.scene->bboxMax);
            he = 0.5f * (ctx.scene->bboxMax - ctx.scene->bboxMin);
            if (he == glm::vec3(0.0f)) he = glm::vec3(1.0f);
        }
    }
    u_.bboxCenterLife  = glm::vec4(c, lifetime);
    u_.bboxExtentSize  = glm::vec4(he, pointSizePx);

    u_.viewportMode    = glm::vec4((float)ctx.width, (float)ctx.height,
                                     (float)currentTrailLength_,
                                     (float)(int)emitter);
    u_.colorStart      = colorStart;
    u_.colorEnd        = colorEnd;
    u_.alphaCurve      = alphaCurve;
    u_.pointOriginTriCount = glm::vec4(pointOrigin, (float)triCount_);
    u_.misc            = glm::vec4(triTotalArea_, (float)ctx.frameIndex,
                                     0.0f, 0.0f);
}

// ---------------------------------------------------------------------------
// Main per-frame render.
// ---------------------------------------------------------------------------

void SDFParticleLayer::render(RenderContext& ctx)
{
    if (state != LayerState::Enabled || opacity <= 0.0f) return;
    if (!ctx.renderer || !ctx.cmdBuf || !ctx.colorTarget) return;
    if (!ctx.scene) return;

    MetalRenderer& renderer = *ctx.renderer;

    buildPipelinesIfNeeded(renderer);
    if (!pipelineReady_) return;

    // Lazy mesh upload (Surface emitter or bbox derivation).
    if (meshDataDirty_) uploadMeshData(renderer, *ctx.scene);

    // Reconcile particle count and trail length with current operator state.
    int desiredN = std::clamp(particleCount, 1024, 1'000'000);
    int desiredK = std::clamp(trailLength,  1,    32);
    reallocBuffersIfNeeded(renderer, desiredN, desiredK);
    if (!particleBuf_) return;

    packUniforms(ctx);

    // ---- 1. Compute pass — update_particles ----
    MTL::ComputeCommandEncoder* cenc = ctx.cmdBuf->computeCommandEncoder();
    if (!cenc) return;
    cenc->setComputePipelineState(updatePipeline_);
    cenc->setBuffer(particleBuf_, 0, 0);
    cenc->setBytes(&u_, sizeof(u_), 1);
    if (triCDFBuf_)   cenc->setBuffer(triCDFBuf_,   0, 2);
    if (triVertsBuf_) cenc->setBuffer(triVertsBuf_, 0, 3);
    if (trailRingBuf_ && trailHeadBuf_) {
        cenc->setBuffer(trailRingBuf_, 0, 4);
        cenc->setBuffer(trailHeadBuf_, 0, 5);
    }
    NS::UInteger threadgroupSize =
        updatePipeline_->maxTotalThreadsPerThreadgroup();
    MTL::Size gridSize    = MTL::Size((NS::UInteger)currentParticleCount_, 1, 1);
    MTL::Size threadGroup = MTL::Size(std::min<NS::UInteger>(threadgroupSize, 256),
                                       1, 1);
    cenc->dispatchThreads(gridSize, threadGroup);
    cenc->endEncoding();

    // ---- 2. Render pass — additive on top of color target ----
    MTL::RenderPassDescriptor* rpd =
        MTL::RenderPassDescriptor::renderPassDescriptor();
    auto* ca = rpd->colorAttachments()->object(0);
    ca->setTexture(ctx.colorTarget);
    ca->setLoadAction(MTL::LoadActionLoad);   // accumulate over structure
    ca->setStoreAction(MTL::StoreActionStore);

    auto* da = rpd->depthAttachment();
    da->setTexture(renderer.depthTexturePublic());  // see INTEGRATION.md
    da->setLoadAction(MTL::LoadActionLoad);
    da->setStoreAction(MTL::StoreActionDontCare);

    MTL::RenderCommandEncoder* renc = ctx.cmdBuf->renderCommandEncoder(rpd);
    if (!renc) return;
    renc->setDepthStencilState(depthReadState_);
    renc->setCullMode(MTL::CullModeNone);

    if (currentTrailLength_ <= 1) {
        // ---- Point sprites ----
        renc->setRenderPipelineState(spritePipeline_);
        renc->setVertexBuffer(particleBuf_, 0, 0);
        renc->setVertexBytes(&u_, sizeof(u_), 1);
        renc->setFragmentBytes(&u_, sizeof(u_), 1);
        renc->drawPrimitives(MTL::PrimitiveTypePoint,
                              (NS::UInteger)0,
                              (NS::UInteger)currentParticleCount_);
    } else if (trailPipeline_ && trailRingBuf_ && trailHeadBuf_) {
        // ---- Trails: instanced line strip, one strip per particle ----
        renc->setRenderPipelineState(trailPipeline_);
        renc->setVertexBuffer(particleBuf_,  0, 0);
        renc->setVertexBytes(&u_, sizeof(u_), 1);
        renc->setFragmentBytes(&u_, sizeof(u_), 1);
        renc->setVertexBuffer(trailRingBuf_, 0, 4);
        renc->setVertexBuffer(trailHeadBuf_, 0, 5);
        renc->drawPrimitives(MTL::PrimitiveTypeLineStrip,
                              (NS::UInteger)0,
                              (NS::UInteger)currentTrailLength_,
                              (NS::UInteger)currentParticleCount_);
    }

    renc->endEncoding();
    framesSinceMeshUpload_++;
}

// ---------------------------------------------------------------------------
// Inspector UI.
// ---------------------------------------------------------------------------

void SDFParticleLayer::drawInspector()
{
    static const char* kModNames[] = {
        "None", "LFO 1", "LFO 2", "LFO 3", "LFO 4",
        "LFO 5", "LFO 6", "LFO 7", "LFO 8"
    };
    static const char* kEmitterNames[] = { "Surface", "Volume", "Point" };

    if (ImGui::CollapsingHeader("Population",
                                  ImGuiTreeNodeFlags_DefaultOpen)) {
        // Log scale slider for particle count (1k .. 1M).
        float logN = std::log10((float)std::max(1, particleCount));
        if (ImGui::SliderFloat("Count (log10)##p", &logN, 3.0f, 6.0f, "%.2f")) {
            particleCount = (int)std::round(std::pow(10.0f, logN));
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%d", particleCount);
        if (ImGui::SliderFloat("Lifetime (s)##p", &lifetime, 1.0f, 60.0f)) {}
        if (ImGui::SliderFloat("Drag##p", &drag, 0.0f, 1.0f)) {}
    }

    if (ImGui::CollapsingHeader("Curl noise (Bridson 2007)",
                                 ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SliderFloat("Scale (1/wavelength)##p", &curlScale,
                             0.05f, 5.0f, "%.2f");
        ImGui::SliderFloat("Amplitude (m/s)##p", &curlAmplitude,
                             0.0f, 10.0f, "%.2f");
        ImGui::SetNextItemWidth(90.0f);
        ImGui::Combo("##camod", &curlAmpModSlot, kModNames, 9);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        ImGui::SliderFloat("Mod depth##cam", &curlAmpModDepth, 0.0f, 10.0f);
    }

    if (ImGui::CollapsingHeader("SDF force (structure attraction)",
                                 ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SliderFloat("Strength (- = repel, + = attract)##p",
                             &sdfStrength, -5.0f, 5.0f, "%.2f");
        ImGui::SliderFloat("Range (m)##p", &sdfRange, 0.05f, 5.0f, "%.2f");
        ImGui::SetNextItemWidth(90.0f);
        ImGui::Combo("##smod", &sdfStrengthModSlot, kModNames, 9);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        ImGui::SliderFloat("Mod depth##sm", &sdfStrengthModDepth, 0.0f, 5.0f);
    }

    if (ImGui::CollapsingHeader("Color",
                                 ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::ColorEdit4("Start##p", &colorStart[0],
            ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_Float);
        ImGui::ColorEdit4("End##p", &colorEnd[0],
            ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_Float);
        ImGui::TextDisabled("Alpha curve (4 pts, t=0 .. t=1):");
        ImGui::SliderFloat4("##ac", &alphaCurve[0], 0.0f, 1.0f, "%.2f");
    }

    if (ImGui::CollapsingHeader("Render",
                                 ImGuiTreeNodeFlags_DefaultOpen)) {
        int e = (int)emitter;
        if (ImGui::Combo("Emitter##p", &e, kEmitterNames, 3)) {
            emitter = (EmitterType)e;
        }
        if (emitter == EmitterType::Point) {
            ImGui::DragFloat3("Origin##p", &pointOrigin[0], 0.05f);
        }
        if (ImGui::SliderInt("Trail length##p", &trailLength, 1, 32)) {}
        if (trailLength == 1) {
            ImGui::SliderFloat("Point size (px)##p",
                                &pointSizePx, 1.0f, 64.0f, "%.1f");
        } else {
            ImGui::TextDisabled("(trails: %d points/particle, max 256k particles)",
                                  trailLength);
        }
    }
}

} // namespace spacegen
