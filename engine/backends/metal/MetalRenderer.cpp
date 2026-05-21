#include "MetalRenderer.h"

#include "../../core/Scene.h"
#include "../../core/Layer.h"
#include "../../core/StructureLayer.h"
#include "../../core/BeamLayer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

namespace spacegen {

namespace {

// MSL shader: PBR forward — GGX + Lambert + Schlick.
// One directional "fill" light from the StructureLayer + up to 16 spot
// lights collected from the bus (BeamLayer instances). Spot lights are
// the projection-mapping primary illumination (origin at camera by
// default), and DO NOT render a visible volume in air — we only see
// their illumination where they hit the structure surface.
constexpr int kMaxSpots = 16;
constexpr const char* kStructurePbrMSL = R"MSL(
#include <metal_stdlib>
using namespace metal;

constant float PI         = 3.14159265359;
constant int   MAX_SPOTS  = 16;

struct VertexIn {
    float3 position [[attribute(0)]];
    float3 normal   [[attribute(1)]];
};

struct VertexOut {
    float4 position [[position]];
    float3 worldPos;
    float3 worldNormal;
};

struct SpotLight {
    float4 posIntensity;   // .xyz pos, .w intensity
    float4 dirRange;       // .xyz dir (FROM light, normalized), .w range
    float4 colorInner;     // .rgb color, .a innerCos
    float4 paramsOuter;    // .x outerCos
};

struct Uniforms {
    float4x4  projection;
    float4x4  view;
    float4x4  model;
    float4    cameraWorldPos;
    float4    baseColorRoughness;   // .rgb base, .a roughness
    float4    metallicAmbient;      // .x metallic, .y ambient, .z spotCount (float)
    float4    lightDirIntensity;    // directional fallback (.xyz dir, .w intensity)
    float4    lightColor;
    SpotLight spots[MAX_SPOTS];
};

vertex VertexOut vs_main(VertexIn in [[stage_in]],
                         constant Uniforms& u [[buffer(1)]])
{
    VertexOut out;
    float4 world = u.model * float4(in.position, 1.0);
    out.position = u.projection * u.view * world;
    out.worldPos = world.xyz;
    out.worldNormal = (u.model * float4(in.normal, 0.0)).xyz;
    return out;
}

// ---- PBR helpers ----
static float3 fresnelSchlick(float cosTheta, float3 F0) {
    float x = 1.0 - cosTheta;
    float x2 = x * x;
    return F0 + (1.0 - F0) * (x2 * x2 * x);
}

static float distributionGGX(float NdotH, float roughness) {
    float a  = roughness * roughness;
    float a2 = a * a;
    float d  = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 1e-5);
}

static float geometrySchlickGGX(float NdotV, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / max(NdotV * (1.0 - k) + k, 1e-5);
}

static float geometrySmith(float NdotV, float NdotL, float roughness) {
    return geometrySchlickGGX(NdotV, roughness)
         * geometrySchlickGGX(NdotL, roughness);
}

// Evaluate one PBR light contribution at a surface point.
static float3 pbrEvalDirect(float3 N, float3 V, float3 L,
                            float3 radiance,
                            float3 baseColor, float roughness, float metallic,
                            float3 F0)
{
    float NdotL = max(dot(N, L), 0.0);
    if (NdotL <= 0.0) return float3(0.0);
    float3 H = normalize(V + L);
    float NdotV = max(dot(N, V), 0.0);
    float NdotH = max(dot(N, H), 0.0);
    float VdotH = max(dot(V, H), 0.0);
    float3 F = fresnelSchlick(VdotH, F0);
    float  D = distributionGGX(NdotH, roughness);
    float  G = geometrySmith(NdotV, NdotL, roughness);
    float3 spec = (D * G * F) / max(4.0 * NdotV * NdotL, 1e-4);
    float3 kd   = (1.0 - F) * (1.0 - metallic);
    float3 diff = kd * baseColor / PI;
    return (diff + spec) * radiance * NdotL;
}

fragment float4 fs_main(VertexOut in [[stage_in]],
                        constant Uniforms& u [[buffer(0)]])
{
    float3 N = normalize(in.worldNormal);
    float3 V = normalize(u.cameraWorldPos.xyz - in.worldPos);

    float3 baseColor = u.baseColorRoughness.rgb;
    float  roughness = max(u.baseColorRoughness.a, 0.04);
    float  metallic  = u.metallicAmbient.r;
    float  ambient   = u.metallicAmbient.g;
    int    spotCount = int(u.metallicAmbient.b);

    float3 F0 = mix(float3(0.04), baseColor, metallic);

    // Directional fallback (preview / fill).
    float3 LoDir = float3(0.0);
    if (u.lightDirIntensity.w > 0.0) {
        float3 L = normalize(-u.lightDirIntensity.xyz);
        float3 radiance = u.lightColor.rgb * u.lightDirIntensity.w;
        LoDir = pbrEvalDirect(N, V, L, radiance,
                              baseColor, roughness, metallic, F0);
    }

    // Spot lights (projection-mapping primary illumination).
    float3 LoSpots = float3(0.0);
    int count = min(spotCount, MAX_SPOTS);
    for (int i = 0; i < count; i++) {
        SpotLight s = u.spots[i];
        float3 sp = s.posIntensity.xyz;
        float  si = s.posIntensity.w;
        float3 sd = normalize(s.dirRange.xyz);
        float  rg = s.dirRange.w;
        float3 sc = s.colorInner.rgb;
        float  ic = s.colorInner.a;
        float  oc = s.paramsOuter.x;

        float3 toLight = sp - in.worldPos;
        float  dist    = length(toLight);
        if (dist > rg) continue;
        float3 L = toLight / max(dist, 1e-4);

        // Cone test: sd points FROM light TO scene; -L points FROM surface
        // TO light. Their dot is high when the surface is inside the cone.
        float spotCos = dot(-L, sd);
        if (spotCos < oc) continue;
        float cone = smoothstep(oc, ic, spotCos);

        // Distance falloff: linear within range (projector-style, no
        // inverse-square — operator wants predictable intensity at typical
        // 5-20m projection distances).
        float rangeFade = saturate(1.0 - dist / rg);

        float3 radiance = sc * si * cone * rangeFade;
        LoSpots += pbrEvalDirect(N, V, L, radiance,
                                  baseColor, roughness, metallic, F0);
    }

    float3 colorLin = ambient * baseColor + LoDir + LoSpots;
    float3 colorOut = pow(max(colorLin, 0.0), float3(1.0 / 2.2));
    return float4(colorOut, 1.0);
}
)MSL";

// CPU mirror of MSL types.
struct GpuSpot {
    glm::vec4 posIntensity;
    glm::vec4 dirRange;
    glm::vec4 colorInner;
    glm::vec4 paramsOuter;
};

struct Uniforms {
    glm::mat4 projection;
    glm::mat4 view;
    glm::mat4 model;
    glm::vec4 cameraWorldPos;
    glm::vec4 baseColorRoughness;
    glm::vec4 metallicAmbient;   // .z = spotCount as float
    glm::vec4 lightDirIntensity;
    glm::vec4 lightColor;
    GpuSpot   spots[kMaxSpots];
};

// (Volumetric beam pass removed in M3-B refresh: spot lights are now
// integrated into the structure shader so we don't render a visible cone
// in air — only the illumination where it lands on the structure.)

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
        if (gm.positionBuffer) gm.positionBuffer->release();
        if (gm.normalBuffer)   gm.normalBuffer->release();
        if (gm.indexBuffer)    gm.indexBuffer->release();
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

    NS::String* src = NS::String::string(kStructurePbrMSL,
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

    // Vertex descriptor:
    //   attribute(0) = position (float3) in buffer(0)
    //   attribute(1) = normal   (float3) in buffer(2)   <- buffer(1) is uniforms
    MTL::VertexDescriptor* vd = MTL::VertexDescriptor::alloc()->init();
    auto* attr0 = vd->attributes()->object(0);
    attr0->setFormat(MTL::VertexFormatFloat3);
    attr0->setOffset(0);
    attr0->setBufferIndex(0);
    auto* attr1 = vd->attributes()->object(1);
    attr1->setFormat(MTL::VertexFormatFloat3);
    attr1->setOffset(0);
    attr1->setBufferIndex(2);
    auto* layout0 = vd->layouts()->object(0);
    layout0->setStride(sizeof(float) * 3);
    layout0->setStepFunction(MTL::VertexStepFunctionPerVertex);
    auto* layout2 = vd->layouts()->object(2);
    layout2->setStride(sizeof(float) * 3);
    layout2->setStepFunction(MTL::VertexStepFunctionPerVertex);

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
        if (gm.positionBuffer) gm.positionBuffer->release();
        if (gm.normalBuffer)   gm.normalBuffer->release();
        if (gm.indexBuffer)    gm.indexBuffer->release();
    }
    gpuMeshes_.clear();
    gpuMeshes_.reserve(scene.meshes.size());

    projection_     = scene.camera.projection;
    view_           = scene.camera.view;
    // Camera world position = 4th column of the camera-to-world matrix.
    cameraWorldPos_ = glm::vec3(scene.camera.world[3]);

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
        gm.positionBuffer = device_->newBuffer(
            m.positions.data(), vbBytes, MTL::ResourceStorageModeShared);

        if (!m.normals.empty()) {
            const size_t nbBytes = m.normals.size() * sizeof(glm::vec3);
            gm.normalBuffer = device_->newBuffer(
                m.normals.data(), nbBytes, MTL::ResourceStorageModeShared);
        } else {
            // Fallback: synthesize a +Z normal buffer so the pipeline still
            // has something to read at buffer(2). Visually wrong but won't
            // crash.
            std::vector<glm::vec3> fakeNormals(m.positions.size(),
                                                glm::vec3(0, 0, 1));
            gm.normalBuffer = device_->newBuffer(
                fakeNormals.data(), fakeNormals.size() * sizeof(glm::vec3),
                MTL::ResourceStorageModeShared);
        }

        const size_t ibBytes = m.indices.size() * sizeof(uint32_t);
        gm.indexBuffer = device_->newBuffer(
            m.indices.data(), ibBytes, MTL::ResourceStorageModeShared);

        const double mbPos = vbBytes / (1024.0 * 1024.0);
        const double mbNrm = (m.normals.empty()
            ? m.positions.size() * sizeof(glm::vec3)
            : m.normals.size() * sizeof(glm::vec3)) / (1024.0 * 1024.0);
        const double mbIdx = ibBytes / (1024.0 * 1024.0);
        std::printf("[MetalRenderer] Uploaded mesh '%s': %zu verts "
                     "(pos %.1f MB + nrm %.1f MB), %u indices (%.1f MB)%s\n",
                     m.name.c_str(),
                     m.positions.size(), mbPos, mbNrm,
                     gm.indexCount, mbIdx,
                     m.normals.empty() ? " [synthesized normals]" : "");

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

void MetalRenderer::renderFrame(RenderContext& ctx, Bus& bus) {
    if (!ctx.cmdBuf || !ctx.colorTarget) return;
    // Ensure depth matches color size.
    onResize(static_cast<int>(ctx.colorTarget->width()),
             static_cast<int>(ctx.colorTarget->height()));
    bus.render(ctx);
}

void MetalRenderer::renderStructureMeshes(
    RenderContext& ctx,
    const StructureLayer& layer,
    const std::vector<const BeamLayer*>& spots)
{
    if (!ctx.cmdBuf || !ctx.colorTarget) return;
    onResize(static_cast<int>(ctx.colorTarget->width()),
             static_cast<int>(ctx.colorTarget->height()));

    MTL::RenderPassDescriptor* rpd =
        MTL::RenderPassDescriptor::renderPassDescriptor();
    auto* colorAttachment = rpd->colorAttachments()->object(0);
    colorAttachment->setTexture(ctx.colorTarget);
    colorAttachment->setLoadAction(MTL::LoadActionClear);
    colorAttachment->setStoreAction(MTL::StoreActionStore);
    colorAttachment->setClearColor(MTL::ClearColor(0.04, 0.05, 0.07, 1.0));

    auto* depthAttachment = rpd->depthAttachment();
    depthAttachment->setTexture(depthTex_);
    depthAttachment->setLoadAction(MTL::LoadActionClear);
    depthAttachment->setStoreAction(MTL::StoreActionDontCare);
    depthAttachment->setClearDepth(1.0);

    MTL::RenderCommandEncoder* enc = ctx.cmdBuf->renderCommandEncoder(rpd);
    if (!enc) return;

    enc->setRenderPipelineState(pipeline_);
    enc->setDepthStencilState(depthState_);
    enc->setCullMode(MTL::CullModeNone);
    enc->setFrontFacingWinding(MTL::WindingCounterClockwise);

    // Pack spot lights once for all meshes.
    int spotCount = std::min(static_cast<int>(spots.size()), kMaxSpots);
    GpuSpot spotsPacked[kMaxSpots]{};
    for (int i = 0; i < spotCount; ++i) {
        const BeamLayer* s = spots[i];
        if (!s) continue;
        glm::vec3 origin    = s->origin;
        glm::vec3 dir       = glm::normalize(s->direction);
        if (s->followCamera) {
            origin = ctx.cameraWorldPos;
            dir    = glm::normalize(ctx.cameraForward);
        }
        float innerRad = s->innerDeg * 3.14159265f / 180.0f;
        float outerRad = s->outerDeg * 3.14159265f / 180.0f;
        if (outerRad < innerRad) outerRad = innerRad;
        float innerCos = std::cos(innerRad);
        float outerCos = std::cos(outerRad);

        spotsPacked[i].posIntensity = glm::vec4(origin,
                                                  s->intensity * s->opacity);
        spotsPacked[i].dirRange     = glm::vec4(dir, s->range);
        spotsPacked[i].colorInner   = glm::vec4(s->color, innerCos);
        spotsPacked[i].paramsOuter  = glm::vec4(outerCos, 0.0f, 0.0f, 0.0f);
    }

    for (const auto& gm : gpuMeshes_) {
        Uniforms u{};
        u.projection         = ctx.projection;
        u.view               = ctx.view;
        u.model              = gm.transform;
        u.cameraWorldPos     = glm::vec4(ctx.cameraWorldPos, 1.0f);
        u.baseColorRoughness = glm::vec4(layer.baseColor, layer.roughness);
        u.metallicAmbient    = glm::vec4(layer.metallic,
                                          layer.ambient,
                                          static_cast<float>(spotCount),
                                          0.0f);
        u.lightDirIntensity  = glm::vec4(glm::normalize(layer.lightDirection),
                                          layer.lightIntensity);
        u.lightColor         = glm::vec4(layer.lightColor, 0.0f);
        std::memcpy(u.spots, spotsPacked, sizeof(GpuSpot) * kMaxSpots);

        enc->setVertexBuffer(gm.positionBuffer, 0, 0);
        enc->setVertexBuffer(gm.normalBuffer,   0, 2);
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
