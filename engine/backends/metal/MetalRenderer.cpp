#include "MetalRenderer.h"

#include "../../core/Scene.h"

#include <cstdio>
#include <stdexcept>
#include <string>

namespace spacegen {

namespace {

// MSL shader source — embedded as a raw string for M2-B simplicity.
// We'll switch to a file-loaded + hot-reload pipeline once we have >1 shader.
constexpr const char* kStructureFlatMSL = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct VertexIn {
    float3 position [[attribute(0)]];
};

struct VertexOut {
    float4 position [[position]];
    float3 worldPos;
};

struct Uniforms {
    float4x4 projection;
    float4x4 view;
    float4x4 model;
    float4   tintColor;
};

vertex VertexOut vs_main(VertexIn in [[stage_in]],
                         constant Uniforms& u [[buffer(1)]])
{
    VertexOut out;
    float4 world = u.model * float4(in.position, 1.0);
    out.position = u.projection * u.view * world;
    out.worldPos = world.xyz;
    return out;
}

fragment float4 fs_main(VertexOut in [[stage_in]],
                        constant Uniforms& u [[buffer(0)]])
{
    return u.tintColor;
}
)MSL";

// Uniforms layout matches MSL `struct Uniforms`. 64*3 + 16 = 208 bytes.
struct Uniforms {
    glm::mat4 projection;
    glm::mat4 view;
    glm::mat4 model;
    glm::vec4 tintColor;
};

} // namespace

MetalRenderer::MetalRenderer(MTL::Device* device, MTL::PixelFormat colorFormat)
    : device_(device), colorFormat_(colorFormat)
{
    if (!device_) {
        throw std::runtime_error("MetalRenderer: device is null");
    }
    buildPipeline();
}

MetalRenderer::~MetalRenderer() {
    for (auto& gm : gpuMeshes_) {
        if (gm.vertexBuffer) gm.vertexBuffer->release();
        if (gm.indexBuffer)  gm.indexBuffer->release();
    }
    if (pipeline_)   pipeline_->release();
    if (depthState_) depthState_->release();
    releaseDepthTexture();
}

void MetalRenderer::releaseDepthTexture() {
    if (depthTex_) {
        depthTex_->release();
        depthTex_ = nullptr;
    }
    depthW_ = depthH_ = 0;
}

void MetalRenderer::buildPipeline() {
    NS::Error* err = nullptr;

    NS::String* src = NS::String::string(kStructureFlatMSL,
                                          NS::UTF8StringEncoding);
    MTL::CompileOptions* opts = MTL::CompileOptions::alloc()->init();
    MTL::Library* lib = device_->newLibrary(src, opts, &err);
    opts->release();
    if (!lib) {
        std::string msg = "MSL compile failed: ";
        if (err) msg += err->localizedDescription()->utf8String();
        throw std::runtime_error(msg);
    }

    MTL::Function* vsFn = lib->newFunction(
        NS::String::string("vs_main", NS::UTF8StringEncoding));
    MTL::Function* fsFn = lib->newFunction(
        NS::String::string("fs_main", NS::UTF8StringEncoding));
    if (!vsFn || !fsFn) {
        if (vsFn) vsFn->release();
        if (fsFn) fsFn->release();
        lib->release();
        throw std::runtime_error("MSL: vs_main or fs_main not found");
    }

    // Vertex descriptor: position at attribute 0, buffer 0.
    MTL::VertexDescriptor* vd = MTL::VertexDescriptor::alloc()->init();
    auto* attr0 = vd->attributes()->object(0);
    attr0->setFormat(MTL::VertexFormatFloat3);
    attr0->setOffset(0);
    attr0->setBufferIndex(0);
    auto* layout0 = vd->layouts()->object(0);
    layout0->setStride(sizeof(float) * 3);
    layout0->setStepFunction(MTL::VertexStepFunctionPerVertex);

    MTL::RenderPipelineDescriptor* pd =
        MTL::RenderPipelineDescriptor::alloc()->init();
    pd->setVertexFunction(vsFn);
    pd->setFragmentFunction(fsFn);
    pd->setVertexDescriptor(vd);
    pd->colorAttachments()->object(0)->setPixelFormat(colorFormat_);
    pd->setDepthAttachmentPixelFormat(MTL::PixelFormatDepth32Float);

    pipeline_ = device_->newRenderPipelineState(pd, &err);
    pd->release();
    vd->release();
    vsFn->release();
    fsFn->release();
    lib->release();

    if (!pipeline_) {
        std::string msg = "RenderPipelineState build failed: ";
        if (err) msg += err->localizedDescription()->utf8String();
        throw std::runtime_error(msg);
    }

    // Depth state: standard less-or-equal with depth write.
    MTL::DepthStencilDescriptor* dsd =
        MTL::DepthStencilDescriptor::alloc()->init();
    dsd->setDepthCompareFunction(MTL::CompareFunctionLess);
    dsd->setDepthWriteEnabled(true);
    depthState_ = device_->newDepthStencilState(dsd);
    dsd->release();
}

void MetalRenderer::loadScene(const Scene& scene) {
    // Drop existing GPU meshes.
    for (auto& gm : gpuMeshes_) {
        if (gm.vertexBuffer) gm.vertexBuffer->release();
        if (gm.indexBuffer)  gm.indexBuffer->release();
    }
    gpuMeshes_.clear();
    gpuMeshes_.reserve(scene.meshes.size());

    projection_ = scene.camera.projection;
    view_       = scene.camera.view;

    for (const auto& m : scene.meshes) {
        if (m.positions.empty() || m.indices.empty()) {
            std::fprintf(stderr,
                "[MetalRenderer] Skipping empty mesh '%s'\n",
                m.name.c_str());
            continue;
        }

        GpuMesh gm;
        gm.name = m.name;
        gm.transform = m.transform;
        gm.indexCount = static_cast<uint32_t>(m.indices.size());

        const size_t vbBytes = m.positions.size() * sizeof(glm::vec3);
        gm.vertexBuffer = device_->newBuffer(
            m.positions.data(), vbBytes, MTL::ResourceStorageModeShared);

        const size_t ibBytes = m.indices.size() * sizeof(uint32_t);
        gm.indexBuffer = device_->newBuffer(
            m.indices.data(), ibBytes, MTL::ResourceStorageModeShared);

        std::printf("[MetalRenderer] Uploaded mesh '%s': %zu verts (%.1f MB), "
                     "%u indices (%.1f MB)\n",
                     m.name.c_str(),
                     m.positions.size(), vbBytes / (1024.0 * 1024.0),
                     gm.indexCount, ibBytes / (1024.0 * 1024.0));

        gpuMeshes_.push_back(gm);
    }
}

void MetalRenderer::onResize(int widthPixels, int heightPixels) {
    if (widthPixels <= 0 || heightPixels <= 0) return;
    if (depthTex_ && widthPixels == depthW_ && heightPixels == depthH_) return;

    releaseDepthTexture();
    MTL::TextureDescriptor* td = MTL::TextureDescriptor::texture2DDescriptor(
        MTL::PixelFormatDepth32Float, widthPixels, heightPixels, false);
    td->setStorageMode(MTL::StorageModePrivate);
    td->setUsage(MTL::TextureUsageRenderTarget);
    depthTex_ = device_->newTexture(td);
    depthW_ = widthPixels;
    depthH_ = heightPixels;
}

void MetalRenderer::renderFrame(MTL::CommandBuffer* cmdBuf,
                                 MTL::Texture* colorTarget,
                                 double /*elapsedSeconds*/)
{
    if (!cmdBuf || !colorTarget) return;
    // Ensure depth matches color size.
    onResize(static_cast<int>(colorTarget->width()),
             static_cast<int>(colorTarget->height()));

    MTL::RenderPassDescriptor* rpd =
        MTL::RenderPassDescriptor::renderPassDescriptor();
    auto* colorAttachment = rpd->colorAttachments()->object(0);
    colorAttachment->setTexture(colorTarget);
    colorAttachment->setLoadAction(MTL::LoadActionClear);
    colorAttachment->setStoreAction(MTL::StoreActionStore);
    // Dark teal background so structure (rendered in red) shows clearly.
    colorAttachment->setClearColor(MTL::ClearColor(0.06, 0.08, 0.10, 1.0));

    auto* depthAttachment = rpd->depthAttachment();
    depthAttachment->setTexture(depthTex_);
    depthAttachment->setLoadAction(MTL::LoadActionClear);
    depthAttachment->setStoreAction(MTL::StoreActionDontCare);
    depthAttachment->setClearDepth(1.0);

    MTL::RenderCommandEncoder* enc = cmdBuf->renderCommandEncoder(rpd);
    if (!enc) return;

    enc->setRenderPipelineState(pipeline_);
    enc->setDepthStencilState(depthState_);
    enc->setCullMode(MTL::CullModeNone); // M2-B: render both sides
    enc->setFrontFacingWinding(MTL::WindingCounterClockwise);

    for (const auto& gm : gpuMeshes_) {
        Uniforms u;
        u.projection = projection_;
        u.view       = view_;
        u.model      = gm.transform;
        u.tintColor  = glm::vec4(0.90f, 0.30f, 0.30f, 1.0f); // tomato red

        enc->setVertexBuffer(gm.vertexBuffer, 0, 0);
        enc->setVertexBytes(&u, sizeof(u), 1);
        enc->setFragmentBytes(&u, sizeof(u), 0);

        enc->drawIndexedPrimitives(
            MTL::PrimitiveTypeTriangle,
            static_cast<NS::UInteger>(gm.indexCount),
            MTL::IndexTypeUInt32,
            gm.indexBuffer,
            static_cast<NS::UInteger>(0));
    }

    enc->endEncoding();
}

size_t MetalRenderer::totalTriangles() const {
    size_t total = 0;
    for (const auto& gm : gpuMeshes_) total += gm.indexCount / 3;
    return total;
}

} // namespace spacegen
