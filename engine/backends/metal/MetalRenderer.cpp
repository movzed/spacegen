#include "MetalRenderer.h"

#include "../../core/Scene.h"

#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace spacegen {

namespace {

// MSL shader: PBR forward — GGX specular + Lambert diffuse + Schlick Fresnel,
// metallic-roughness workflow. Single directional light for M2-C1; M2-C2
// generalizes to an array of mixed light types.
constexpr const char* kStructurePbrMSL = R"MSL(
#include <metal_stdlib>
using namespace metal;

constant float PI = 3.14159265359;

struct VertexIn {
    float3 position [[attribute(0)]];
    float3 normal   [[attribute(1)]];
};

struct VertexOut {
    float4 position [[position]];
    float3 worldPos;
    float3 worldNormal;
};

struct Uniforms {
    float4x4 projection;
    float4x4 view;
    float4x4 model;
    float4   cameraWorldPos;       // .xyz used
    float4   baseColorRoughness;   // .rgb = base color (linear),  .a = roughness
    float4   metallicAmbient;      // .r = metallic, .g = ambient
    float4   lightDirIntensity;    // .xyz = world direction (FROM light), .w = intensity
    float4   lightColor;           // .rgb = linear radiance, .a unused
};

vertex VertexOut vs_main(VertexIn in [[stage_in]],
                         constant Uniforms& u [[buffer(1)]])
{
    VertexOut out;
    float4 world = u.model * float4(in.position, 1.0);
    out.position = u.projection * u.view * world;
    out.worldPos = world.xyz;
    // Uniform-scale-safe normal transform (assumes mostly uniform scale —
    // good enough for M2-C1; switch to inverse-transpose if non-uniform
    // scaled assets land).
    float3 wn = (u.model * float4(in.normal, 0.0)).xyz;
    out.worldNormal = wn;
    return out;
}

// ---- PBR helpers (metallic-roughness, GGX + Smith + Schlick) ----

static float3 fresnelSchlick(float cosTheta, float3 F0) {
    float x = 1.0 - cosTheta;
    float x2 = x * x;
    return F0 + (1.0 - F0) * (x2 * x2 * x); // x^5
}

static float distributionGGX(float NdotH, float roughness) {
    float a  = roughness * roughness;
    float a2 = a * a;
    float d  = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 1e-5);
}

static float geometrySchlickGGX(float NdotV, float roughness) {
    // Karis remap (UE4) — direct lighting variant
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / max(NdotV * (1.0 - k) + k, 1e-5);
}

static float geometrySmith(float NdotV, float NdotL, float roughness) {
    return geometrySchlickGGX(NdotV, roughness)
         * geometrySchlickGGX(NdotL, roughness);
}

fragment float4 fs_main(VertexOut in [[stage_in]],
                        constant Uniforms& u [[buffer(0)]])
{
    float3 N = normalize(in.worldNormal);
    float3 V = normalize(u.cameraWorldPos.xyz - in.worldPos);
    // Light direction in shader convention: TOWARDS the light (negate the
    // direction the light is travelling).
    float3 L = normalize(-u.lightDirIntensity.xyz);
    float3 H = normalize(V + L);

    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 0.0);
    float NdotH = max(dot(N, H), 0.0);
    float VdotH = max(dot(V, H), 0.0);

    float3 baseColor = u.baseColorRoughness.rgb;
    float  roughness = max(u.baseColorRoughness.a, 0.04);
    float  metallic  = u.metallicAmbient.r;
    float  ambient   = u.metallicAmbient.g;

    // F0: 4% for dielectrics, baseColor-tinted for metals.
    float3 F0 = mix(float3(0.04), baseColor, metallic);

    float3 F = fresnelSchlick(VdotH, F0);
    float  D = distributionGGX(NdotH, roughness);
    float  G = geometrySmith(NdotV, NdotL, roughness);

    float3 spec = (D * G * F) / max(4.0 * NdotV * NdotL, 1e-4);
    float3 kd = (1.0 - F) * (1.0 - metallic);
    float3 diff = kd * baseColor / PI;

    float3 radiance = u.lightColor.rgb * u.lightDirIntensity.w;
    float3 Lo       = (diff + spec) * radiance * NdotL;

    float3 colorLin = ambient * baseColor + Lo;
    // Cheap gamma encode for the BGRA8Unorm swapchain (we don't have an sRGB
    // attachment yet). Replace with proper sRGB pipeline output later.
    float3 colorOut = pow(max(colorLin, 0.0), float3(1.0 / 2.2));

    return float4(colorOut, 1.0);
}
)MSL";

// CPU mirror of MSL Uniforms. Use vec4s everywhere so the std430-ish layout
// matches Metal's 16-byte alignment without padding gymnastics.
struct Uniforms {
    glm::mat4 projection;
    glm::mat4 view;
    glm::mat4 model;
    glm::vec4 cameraWorldPos;
    glm::vec4 baseColorRoughness;
    glm::vec4 metallicAmbient;
    glm::vec4 lightDirIntensity;
    glm::vec4 lightColor;
};

// ---- Volumetric beam pass ----------------------------------------------
// Fullscreen triangle (no vertex buffer; uses [[vertex_id]]). Reads NDC from
// vertex_id, marches a ray from the camera through each pixel and integrates
// scattering whenever it falls inside a world-space cone.

constexpr const char* kVolumetricBeamMSL = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct BeamUniforms {
    float4x4 invViewProj;       // unprojects NDC -> world
    float4   cameraWorldPos;    // .xyz
    float4   beamOrigin;        // .xyz
    float4   beamDirIntensity;  // .xyz dir (normalized), .w intensity
    float4   beamColor;         // .rgb linear
    float4   beamParams;        // .x cone cos (inner), .y range, .z falloff, .w steps
};

struct V2F {
    float4 position [[position]];
    float2 ndc;
};

vertex V2F vs_beam(uint vid [[vertex_id]]) {
    V2F out;
    // Big triangle: (-1,-1), (3,-1), (-1,3)
    float2 p;
    p.x = (vid == 1) ? 3.0 : -1.0;
    p.y = (vid == 2) ? 3.0 : -1.0;
    out.position = float4(p, 0.0, 1.0);
    out.ndc = p;
    return out;
}

fragment float4 fs_beam(V2F in [[stage_in]],
                        constant BeamUniforms& u [[buffer(0)]])
{
    // Reconstruct world-space ray for this pixel.
    // Near plane: NDC z = 0 in Metal. Far plane: NDC z = 1.
    float4 nearH = u.invViewProj * float4(in.ndc, 0.0, 1.0);
    float4 farH  = u.invViewProj * float4(in.ndc, 1.0, 1.0);
    float3 nearW = nearH.xyz / nearH.w;
    float3 farW  = farH.xyz  / farH.w;

    float3 rayOrigin = nearW;
    float3 rayDir    = normalize(farW - nearW);

    float3 beamPos   = u.beamOrigin.xyz;
    float3 beamDir   = normalize(u.beamDirIntensity.xyz);
    float  beamInt   = u.beamDirIntensity.w;
    float3 beamCol   = u.beamColor.rgb;
    float  coneCos   = u.beamParams.x;
    float  beamRange = u.beamParams.y;
    float  falloff   = u.beamParams.z;
    int    steps     = max(int(u.beamParams.w), 4);

    // March from camera near plane out to range.
    float marchEnd = beamRange * 2.5;  // generous; falloff handles attenuation
    float dt = marchEnd / float(steps);
    float3 accum = float3(0.0);

    for (int i = 0; i < steps; i++) {
        float t  = (float(i) + 0.5) * dt;
        float3 p = rayOrigin + rayDir * t;

        // Position relative to beam origin
        float3 d = p - beamPos;
        float along = dot(d, beamDir);
        if (along <= 0.0 || along > beamRange) continue;

        float3 perp     = d - along * beamDir;
        float perpLen   = length(perp);

        // Cone radius at this depth (using tan from cone cos -> half-angle).
        // tan = sqrt(1 - cos^2) / cos
        float coneTan   = sqrt(max(1.0 - coneCos * coneCos, 0.0)) / max(coneCos, 1e-4);
        float coneRad   = along * coneTan;
        if (perpLen > coneRad) continue;

        // Radial profile (falls off from center to edge)
        float radial    = pow(saturate(1.0 - perpLen / max(coneRad, 1e-4)), falloff);
        // Range falloff
        float rangeFade = saturate(1.0 - along / beamRange);
        // Soft origin start (avoid clipping right at the beam origin)
        float originFade = smoothstep(0.0, beamRange * 0.08, along);

        accum += beamCol * (radial * rangeFade * originFade) * (beamInt * dt);
    }

    return float4(accum, 1.0);
}
)MSL";

struct BeamUniformsCpu {
    glm::mat4 invViewProj;
    glm::vec4 cameraWorldPos;
    glm::vec4 beamOrigin;
    glm::vec4 beamDirIntensity;
    glm::vec4 beamColor;
    glm::vec4 beamParams;  // x=coneCos, y=range, z=falloff, w=steps
};

} // namespace

MetalRenderer::MetalRenderer(MTL::Device* device, MTL::PixelFormat colorFormat)
    : device_(device), colorFormat_(colorFormat)
{
    if (!device_) {
        throw std::runtime_error("MetalRenderer: device is null");
    }
    buildPipeline();
    buildBeamPipeline();
}

MetalRenderer::~MetalRenderer() {
    for (auto& gm : gpuMeshes_) {
        if (gm.positionBuffer) gm.positionBuffer->release();
        if (gm.normalBuffer)   gm.normalBuffer->release();
        if (gm.indexBuffer)    gm.indexBuffer->release();
    }
    if (pipeline_)     pipeline_->release();
    if (beamPipeline_) beamPipeline_->release();
    if (depthState_)   depthState_->release();
    releaseDepthTexture();
}

void MetalRenderer::buildBeamPipeline() {
    NS::Error* err = nullptr;
    NS::String* src = NS::String::string(kVolumetricBeamMSL,
                                          NS::UTF8StringEncoding);
    MTL::CompileOptions* opts = MTL::CompileOptions::alloc()->init();
    MTL::Library* lib = device_->newLibrary(src, opts, &err);
    opts->release();
    if (!lib) {
        std::string msg = "Beam MSL compile failed: ";
        if (err) msg += err->localizedDescription()->utf8String();
        throw std::runtime_error(msg);
    }
    MTL::Function* vsFn = lib->newFunction(
        NS::String::string("vs_beam", NS::UTF8StringEncoding));
    MTL::Function* fsFn = lib->newFunction(
        NS::String::string("fs_beam", NS::UTF8StringEncoding));

    MTL::RenderPipelineDescriptor* pd =
        MTL::RenderPipelineDescriptor::alloc()->init();
    pd->setVertexFunction(vsFn);
    pd->setFragmentFunction(fsFn);
    pd->setVertexDescriptor(nullptr); // fullscreen triangle, no vbo
    auto* colorAtt = pd->colorAttachments()->object(0);
    colorAtt->setPixelFormat(colorFormat_);
    // Additive blending: dst = src + dst (color), alpha stays 1.
    colorAtt->setBlendingEnabled(true);
    colorAtt->setRgbBlendOperation(MTL::BlendOperationAdd);
    colorAtt->setSourceRGBBlendFactor(MTL::BlendFactorOne);
    colorAtt->setDestinationRGBBlendFactor(MTL::BlendFactorOne);
    colorAtt->setAlphaBlendOperation(MTL::BlendOperationAdd);
    colorAtt->setSourceAlphaBlendFactor(MTL::BlendFactorZero);
    colorAtt->setDestinationAlphaBlendFactor(MTL::BlendFactorOne);
    // No depth attachment for the beam pass: we DON'T want depth-test or
    // depth-write here. The pass is purely additive overlay.
    pd->setDepthAttachmentPixelFormat(MTL::PixelFormatInvalid);

    beamPipeline_ = device_->newRenderPipelineState(pd, &err);
    pd->release();
    vsFn->release();
    fsFn->release();
    lib->release();

    if (!beamPipeline_) {
        std::string msg = "Beam RenderPipelineState build failed: ";
        if (err) msg += err->localizedDescription()->utf8String();
        throw std::runtime_error(msg);
    }
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
        u.projection         = projection_;
        u.view               = view_;
        u.model              = gm.transform;
        u.cameraWorldPos     = glm::vec4(cameraWorldPos_, 1.0f);
        u.baseColorRoughness = glm::vec4(baseColor, roughness);
        u.metallicAmbient    = glm::vec4(metallic, ambient, 0.0f, 0.0f);
        u.lightDirIntensity  = glm::vec4(glm::normalize(lightDirection),
                                          lightIntensity);
        u.lightColor         = glm::vec4(lightColor, 0.0f);

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

    // ---- Beam pass (additive overlay on the same color target) ----
    if (beamEnabled && beamPipeline_) {
        MTL::RenderPassDescriptor* brpd =
            MTL::RenderPassDescriptor::renderPassDescriptor();
        auto* bcol = brpd->colorAttachments()->object(0);
        bcol->setTexture(colorTarget);
        bcol->setLoadAction(MTL::LoadActionLoad);
        bcol->setStoreAction(MTL::StoreActionStore);
        // No depth attachment: beam pass is purely additive overlay.

        MTL::RenderCommandEncoder* be = cmdBuf->renderCommandEncoder(brpd);
        if (be) {
            be->setRenderPipelineState(beamPipeline_);

            BeamUniformsCpu bu;
            bu.invViewProj      = glm::inverse(projection_ * view_);
            bu.cameraWorldPos   = glm::vec4(cameraWorldPos_, 1.0f);
            bu.beamOrigin       = glm::vec4(beamOrigin, 0.0f);
            bu.beamDirIntensity = glm::vec4(glm::normalize(beamDirection),
                                             beamIntensity);
            bu.beamColor        = glm::vec4(beamColor, 0.0f);
            float coneRad       = beamConeDeg * 3.14159265f / 180.0f;
            float coneCos       = std::cos(coneRad);
            bu.beamParams       = glm::vec4(coneCos, beamRange,
                                             beamFalloff,
                                             static_cast<float>(beamSteps));

            be->setVertexBytes(&bu, sizeof(bu), 0);
            be->setFragmentBytes(&bu, sizeof(bu), 0);
            be->drawPrimitives(MTL::PrimitiveTypeTriangle,
                                static_cast<NS::UInteger>(0),
                                static_cast<NS::UInteger>(3));
            be->endEncoding();
        }
    }
}

size_t MetalRenderer::totalTriangles() const {
    size_t total = 0;
    for (const auto& gm : gpuMeshes_) total += gm.indexCount / 3;
    return total;
}

} // namespace spacegen
