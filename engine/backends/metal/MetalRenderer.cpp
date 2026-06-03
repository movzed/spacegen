#include "MetalRenderer.h"

#include "../../core/Scene.h"
#include "../../core/Layer.h"
#include "../../core/StructureLayer.h"
#include "../../core/BeamLayer.h"
#include "../../core/DirectionalLightLayer.h"
#include "../../core/ModulatorBank.h"
#include "../../effects/light_cloner/LightClonerLayer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

namespace spacegen {

namespace {

// MSL shader: PBR forward — GGX + Lambert + Schlick.
// Up to MAX_DIRS directional lights + MAX_SPOTS spot lights, all
// collected from the bus. NO volumetric render — spot lights are
// projection-mapping style: only visible where they hit the surface.
constexpr int kMaxSpots = 64;   // up to ~4 rigs × 8 fixtures, plus
                                  // LightClonerLayer virtual-spot expansion
                                  // (Notch Cloner pattern, up to 64 clones).
constexpr int kMaxDirs  = 4;
constexpr const char* kStructurePbrMSL = R"MSL(
#include <metal_stdlib>
using namespace metal;

constant float PI         = 3.14159265359;
constant int   MAX_SPOTS  = 64;
constant int   MAX_DIRS   = 4;

struct VertexIn {
    float3 position [[attribute(0)]];
    float3 normal   [[attribute(1)]];
    float2 uv       [[attribute(2)]];   // UV0 — materials
    float2 uv1      [[attribute(3)]];   // UV1 — Syphon overlay
};

struct VertexOut {
    float4 position [[position]];
    float3 worldPos;
    float3 worldNormal;
    float2 uv;
    float2 uv1;
};

struct SpotLight {
    float4 posIntensity;   // .xyz pos, .w intensity
    float4 dirRange;       // .xyz dir (FROM light, normalized), .w range
    float4 colorInner;     // .rgb color, .a innerCos
    float4 paramsOuter;    // .x outerCos
};

struct DirLight {
    float4 dirIntensity;   // .xyz dir (FROM light), .w intensity
    float4 color;          // .rgb
};

struct Uniforms {
    float4x4  projection;
    float4x4  view;
    float4x4  model;
    float4    cameraWorldPos;
    float4    baseColorRoughness;   // .rgb operator tint, .a operator roughness multiplier
    float4    metallicCounts;       // .x metallic operator, .z spotCount, .w dirCount
    float4    ambientColor;         // .rgb ambient fill, .a unused
    float4    modeFlags;            // .x emitLightsOnly, .y heatmapEnabled,
                                     // .z heatmapMetric (0=ratio,1=symDir),
                                     // .w heatmapUV (0=uv0, 1=uv1)
    float4    matBaseColor;         // .rgba material baseColorFactor (texture multiplier)
    float4    matEmissive;          // .rgb material emissiveFactor
    float4    matMR;                // .x metallicFactor, .y roughnessFactor
    float4    fx;                   // .x displaceAmount, .y displaceScale, .z twistAmount
    float4    syphonMixTint;        // .x mix (0..1), .yzw tint RGB
    float4    syphonParams;         // .x useAtlasUVs (0/1), .z flipY (0/1)
    DirLight  dirs[MAX_DIRS];
    SpotLight spots[MAX_SPOTS];
};

// Hash-based pseudo-random scalar in [-1, 1] from a 3D position.
static float vsHash(float3 p) {
    float3 s = sin(p * float3(127.1, 311.7, 74.7));
    return fract(s.x + s.y + s.z) * 2.0 - 1.0;
}

vertex VertexOut vs_main(VertexIn in [[stage_in]],
                         constant Uniforms& u [[buffer(1)]])
{
    VertexOut out;

    // ---- Mesh-effect displacement (local space) ----
    float3 pos = in.position;
    float displaceAmount = u.fx.x;
    float displaceScale  = u.fx.y;
    float twistAmount    = u.fx.z;

    if (twistAmount != 0.0) {
        // Twist around the Z axis as a function of height.
        float angle = pos.z * twistAmount;
        float ca = cos(angle), sa = sin(angle);
        pos = float3(ca * pos.x - sa * pos.y,
                     sa * pos.x + ca * pos.y,
                     pos.z);
    }
    if (displaceAmount != 0.0) {
        float n = vsHash(pos * displaceScale);
        // Move outward along the normal, modulated by noise.
        pos += in.normal * displaceAmount * (1.0 + n) * 0.5;
    }

    float4 world = u.model * float4(pos, 1.0);
    out.position = u.projection * u.view * world;
    out.worldPos = world.xyz;
    out.worldNormal = (u.model * float4(in.normal, 0.0)).xyz;
    out.uv  = in.uv;
    out.uv1 = in.uv1;
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
                        constant Uniforms& u [[buffer(0)]],
                        texture2d<float> baseColorMap [[texture(0)]],
                        texture2d<float> mrMap        [[texture(1)]],
                        texture2d<float> emissiveMap  [[texture(2)]],
                        texture2d<float> syphonMap    [[texture(3)]],
                        sampler          smp          [[sampler(0)]])
{
    float3 N = normalize(in.worldNormal);
    float3 V = normalize(u.cameraWorldPos.xyz - in.worldPos);

    // ---- Tier 2: Stretch heatmap diagnostic ----------------------------
    // When enabled, the structure renders as a viridis-like heatmap of per-
    // fragment UV distortion. We derive the singular values σ₁, σ₂ of the
    // Jacobian (UV → world) from screen-space derivatives — no precomputed
    // tables needed. Two metrics are supported (per modeFlags.z):
    //   0: stretch ratio   = max(σ₁/σ₂, σ₂/σ₁)        ideal 1.0
    //   1: symmetric Dir.  = σ₁² + σ₂² + σ₁⁻² + σ₂⁻²  ideal 4.0
    // modeFlags.w selects which UV layer to measure (0=uv0, 1=uv1).
    // Returns early bypassing lighting so the operator sees a flat diagnostic
    // overlay — easier to reason about than a lit version.
    if (u.modeFlags.y > 0.5) {
        int    metric = int(u.modeFlags.z + 0.5);
        int    uvSel  = int(u.modeFlags.w + 0.5);
        float2 selUv  = (uvSel == 1) ? in.uv1 : in.uv;
        float2 dUx = dfdx(selUv);
        float2 dUy = dfdy(selUv);
        float3 dWx = dfdx(in.worldPos);
        float3 dWy = dfdy(in.worldPos);
        float  det = dUx.x * dUy.y - dUx.y * dUy.x;
        float3 heat = float3(1.0, 0.0, 0.0);   // degenerate UV → pure red
        if (abs(det) > 1e-12) {
            float invDet = 1.0 / det;
            float3 J_u = ( dUy.y * dWx - dUx.y * dWy) * invDet;
            float3 J_v = (-dUy.x * dWx + dUx.x * dWy) * invDet;
            float a = dot(J_u, J_u);
            float b = dot(J_u, J_v);
            float c = dot(J_v, J_v);
            float tr   = a + c;
            float diff = (a - c) * 0.5;
            float disc = sqrt(max(diff*diff + b*b, 0.0));
            float l1 = max(tr * 0.5 + disc, 1e-12);
            float l2 = max(tr * 0.5 - disc, 1e-12);
            float s1 = sqrt(l1), s2 = sqrt(l2);
            float distortion = (metric == 1)
                ? (l1 + l2 + 1.0/l1 + 1.0/l2)
                : max(s1/max(s2, 1e-6), s2/max(s1, 1e-6));
            float ideal = (metric == 1) ? 4.0 : 1.0;
            float t = saturate((distortion - ideal) / 4.0);
            heat = mix(mix(mix(
                float3(1.00, 1.00, 1.00),
                float3(0.20, 0.85, 0.95),
                saturate(t / 0.25)),
                float3(0.30, 0.85, 0.30),
                saturate((t - 0.25) / 0.25)),
                float3(0.95, 0.30, 0.20),
                saturate((t - 0.50) / 0.50));
        }
        return float4(heat, 1.0);
    }

    // Sample material textures (default-bound to 1x1 fallbacks when material
    // has no texture for that channel).
    float4 baseTex = baseColorMap.sample(smp, in.uv);
    float4 mrTex   = mrMap       .sample(smp, in.uv);
    float3 emiTex  = emissiveMap .sample(smp, in.uv).rgb;

    // Live Syphon texture: mixed into the base color according to mix slider.
    // Skipped entirely when mix == 0 (no publisher, or layer disabled) so
    // we don't pay the dfdx/sample cost on every fragment when there's
    // nothing to draw. This reclaims the ~30-50% fps drop introduced by
    // the auto-hybrid sampling.
    //
    // When active, hybrid rule:
    //   - dfdx(in.uv) + dfdy(in.uv) ~ 0 → face has degenerate UV0 → use UV1.
    //   - Otherwise → use UV0 (preserve PRT_UVW behaviour on the mask).
    float  syphonMix    = saturate(u.syphonMixTint.x);
    float3 syphonSample = float3(0.0);
    if (syphonMix > 1e-4) {
        bool   syphonFlipY = u.syphonParams.z > 0.5;
        float  projMixMax  = u.syphonParams.x;
        float  flatThresh  = max(u.syphonParams.y, 1e-4);

        // UV0 gradient — used to pick UV0 vs UV1 (the existing hybrid).
        float2 uvDx   = dfdx(in.uv);
        float2 uvDy   = dfdy(in.uv);
        float  uvGrad = abs(uvDx.x) + abs(uvDx.y)
                      + abs(uvDy.x) + abs(uvDy.y);
        bool   useUv0 = (uvGrad > 1e-6);

        // Atlas / UV0 sample (the "texture" path).
        float2 atlasUv = useUv0 ? in.uv : in.uv1;
        if (syphonFlipY) atlasUv.y = 1.0 - atlasUv.y;
        float3 atlasSample = syphonMap.sample(smp, atlasUv).rgb;

        // Projector-on-flat: ONLY active where UV0 is degenerate (i.e.,
        // we'd otherwise sample atlas), AND the surface is locally flat
        // in screen-space (low normal curvature). Mixes between the atlas
        // sample (good for curved 3D detail) and a projector-NDC sample
        // (1:1 video → flat surface). Curved areas (mask UV0, or atlas
        // areas with curvature) keep their texture mapping untouched.
        float3 finalSample = atlasSample;
        if (!useUv0 && projMixMax > 1e-4) {
            float3 dNx = dfdx(in.worldNormal);
            float3 dNy = dfdy(in.worldNormal);
            float  normalCurv = abs(dNx.x) + abs(dNx.y) + abs(dNx.z)
                              + abs(dNy.x) + abs(dNy.y) + abs(dNy.z);
            // 0 at curvature ≥ threshold, 1 at curvature 0.
            float flatness = 1.0 - saturate(normalCurv / flatThresh);
            float projMix  = projMixMax * flatness;
            if (projMix > 1e-4) {
                // Compute projector NDC from worldPos.
                float4 cp = u.projection * u.view * float4(in.worldPos, 1.0);
                if (cp.w > 1e-4) {
                    float2 ndc = cp.xy / cp.w;
                    float2 puv = ndc * 0.5 + 0.5;
                    puv.y = 1.0 - puv.y;
                    if (syphonFlipY) puv.y = 1.0 - puv.y;
                    if (puv.x >= 0.0 && puv.x <= 1.0
                        && puv.y >= 0.0 && puv.y <= 1.0) {
                        float3 projSample = syphonMap.sample(smp, puv).rgb;
                        finalSample = mix(atlasSample, projSample, projMix);
                    }
                }
            }
        }
        syphonSample = finalSample * u.syphonMixTint.yzw;
    }

    // Compose final material values:
    //   final = operator_tint × material_factor × texture, then mix the
    //   Syphon overlay on top of that for the base color channel.
    float3 baseMat   = u.baseColorRoughness.rgb
                     * u.matBaseColor.rgb
                     * baseTex.rgb;
    float3 baseColor = mix(baseMat, syphonSample, syphonMix);
    float  roughness = max(u.baseColorRoughness.a
                          * u.matMR.y
                          * mrTex.g, 0.04);
    float  metallic  = u.metallicCounts.r
                     * u.matMR.x
                     * mrTex.b;
    float3 emission  = u.matEmissive.rgb * emiTex;
    float3 ambient   = u.ambientColor.rgb;
    int    spotCount = int(u.metallicCounts.b);
    int    dirCount  = int(u.metallicCounts.a);

    float3 F0 = mix(float3(0.04), baseColor, metallic);

    // Directional lights.
    float3 LoDir = float3(0.0);
    int dn = min(dirCount, MAX_DIRS);
    for (int i = 0; i < dn; i++) {
        DirLight d = u.dirs[i];
        if (d.dirIntensity.w <= 0.0) continue;
        float3 L = normalize(-d.dirIntensity.xyz);
        float3 radiance = d.color.rgb * d.dirIntensity.w;
        LoDir += pbrEvalDirect(N, V, L, radiance,
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

    float3 totalLight = LoDir + LoSpots + emission;
    bool   lightsOnly = u.modeFlags.x > 0.5;

    if (lightsOnly) {
        // Lights-only / composite mode: alpha tracks light brightness, base
        // color and ambient suppressed. Premultiplied output so it composites
        // cleanly in Resolume on top of Blender plates.
        float lum   = dot(totalLight, float3(0.2126, 0.7152, 0.0722));
        float alpha = saturate(lum);
        float3 colorOut = pow(max(totalLight, 0.0), float3(1.0 / 2.2));
        return float4(colorOut * alpha, alpha);
    }

    float3 colorLin = ambient * baseColor + totalLight;
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

struct GpuDir {
    glm::vec4 dirIntensity;
    glm::vec4 color;
};

struct Uniforms {
    glm::mat4 projection;
    glm::mat4 view;
    glm::mat4 model;
    glm::vec4 cameraWorldPos;
    glm::vec4 baseColorRoughness;
    glm::vec4 metallicCounts;    // .x metallic, .z spotCount, .w dirCount
    glm::vec4 ambientColor;      // .rgb ambient fill
    glm::vec4 modeFlags;         // .x emitLightsOnly (0/1)
    glm::vec4 matBaseColor;      // material baseColorFactor (RGBA)
    glm::vec4 matEmissive;       // .rgb emissiveFactor
    glm::vec4 matMR;             // .x metallicFactor, .y roughnessFactor
    glm::vec4 fx;                // .x displace, .y displaceScale, .z twist
    glm::vec4 syphonMixTint;     // .x mix, .yzw tint
    glm::vec4 syphonParams;      // .x useAtlasUVs (0/1), .z flipY (0/1)
    GpuDir    dirs[kMaxDirs];
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
    buildDefaultTexturesAndSampler();
    buildPipeline();
}

MetalRenderer::~MetalRenderer() {
    for (auto& gm : gpuMeshes_) {
        if (gm.positionBuffer) gm.positionBuffer->release();
        if (gm.normalBuffer)   gm.normalBuffer->release();
        if (gm.uvBuffer)       gm.uvBuffer->release();
        if (gm.uv1Buffer)      gm.uv1Buffer->release();
        if (gm.indexBuffer)    gm.indexBuffer->release();
    }
    releaseTexturePool();
    if (defaultWhite_)  defaultWhite_->release();
    if (defaultLinear_) defaultLinear_->release();
    if (defaultBlack_)  defaultBlack_->release();
    if (linearSampler_) linearSampler_->release();
    if (pipeline_)   pipeline_->release();
    if (depthState_) depthState_->release();
    releaseDepthTexture();
}

void MetalRenderer::releaseTexturePool() {
    for (auto* t : texturePool_) {
        if (t) t->release();
    }
    texturePool_.clear();
    gpuMaterials_.clear();
}

MTL::Texture* MetalRenderer::createTextureFromRgba(
    const uint8_t* rgba, int width, int height,
    bool isSRGB, const char* /*debugName*/)
{
    if (width <= 0 || height <= 0 || !rgba) return nullptr;
    MTL::PixelFormat fmt = isSRGB
        ? MTL::PixelFormatRGBA8Unorm_sRGB
        : MTL::PixelFormatRGBA8Unorm;
    MTL::TextureDescriptor* td = MTL::TextureDescriptor::texture2DDescriptor(
        fmt, width, height, false);
    td->setStorageMode(MTL::StorageModeShared);
    td->setUsage(MTL::TextureUsageShaderRead);
    MTL::Texture* tex = device_->newTexture(td);
    if (!tex) return nullptr;
    MTL::Region region = MTL::Region::Make2D(0, 0, width, height);
    tex->replaceRegion(region, 0, rgba,
                        static_cast<NS::UInteger>(width) * 4);
    return tex;
}

void MetalRenderer::buildDefaultTexturesAndSampler() {
    // 1x1 fallback textures so the shader's texture binding never sees null.
    static const uint8_t white[4]  = {255, 255, 255, 255};
    static const uint8_t black[4]  = {  0,   0,   0, 255};
    defaultWhite_  = createTextureFromRgba(white, 1, 1, true,  "default_white_srgb");
    defaultLinear_ = createTextureFromRgba(white, 1, 1, false, "default_white_lin");
    defaultBlack_  = createTextureFromRgba(black, 1, 1, false, "default_black");

    // Linear sampler with mipmap-ready settings (we don't generate mipmaps
    // yet — small perf hit, fine for v1).
    MTL::SamplerDescriptor* sd = MTL::SamplerDescriptor::alloc()->init();
    sd->setMinFilter(MTL::SamplerMinMagFilterLinear);
    sd->setMagFilter(MTL::SamplerMinMagFilterLinear);
    sd->setMipFilter(MTL::SamplerMipFilterLinear);
    sd->setSAddressMode(MTL::SamplerAddressModeRepeat);
    sd->setTAddressMode(MTL::SamplerAddressModeRepeat);
    sd->setMaxAnisotropy(8);
    linearSampler_ = device_->newSamplerState(sd);
    sd->release();

    // Default material (used when a mesh has no material): factor 1, no
    // emission, dielectric medium roughness.
    defaultMaterial_.baseColorTex    = defaultWhite_;
    defaultMaterial_.mrTex           = defaultLinear_;
    defaultMaterial_.emissiveTex     = defaultBlack_;
    defaultMaterial_.baseColorFactor = glm::vec4(1.0f);
    defaultMaterial_.emissiveFactor  = glm::vec3(0.0f);
    defaultMaterial_.metallicFactor  = 0.0f;
    defaultMaterial_.roughnessFactor = 1.0f;
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
    //   attribute(2) = uv0      (float2) in buffer(3)   (materials)
    //   attribute(3) = uv1      (float2) in buffer(4)   (Syphon overlay)
    MTL::VertexDescriptor* vd = MTL::VertexDescriptor::alloc()->init();
    auto* attr0 = vd->attributes()->object(0);
    attr0->setFormat(MTL::VertexFormatFloat3);
    attr0->setOffset(0);
    attr0->setBufferIndex(0);
    auto* attr1 = vd->attributes()->object(1);
    attr1->setFormat(MTL::VertexFormatFloat3);
    attr1->setOffset(0);
    attr1->setBufferIndex(2);
    auto* attr2 = vd->attributes()->object(2);
    attr2->setFormat(MTL::VertexFormatFloat2);
    attr2->setOffset(0);
    attr2->setBufferIndex(3);
    auto* attr3 = vd->attributes()->object(3);
    attr3->setFormat(MTL::VertexFormatFloat2);
    attr3->setOffset(0);
    attr3->setBufferIndex(4);
    auto* layout0 = vd->layouts()->object(0);
    layout0->setStride(sizeof(float) * 3);
    layout0->setStepFunction(MTL::VertexStepFunctionPerVertex);
    auto* layout2 = vd->layouts()->object(2);
    layout2->setStride(sizeof(float) * 3);
    layout2->setStepFunction(MTL::VertexStepFunctionPerVertex);
    auto* layout3 = vd->layouts()->object(3);
    layout3->setStride(sizeof(float) * 2);
    layout3->setStepFunction(MTL::VertexStepFunctionPerVertex);
    auto* layout4 = vd->layouts()->object(4);
    layout4->setStride(sizeof(float) * 2);
    layout4->setStepFunction(MTL::VertexStepFunctionPerVertex);

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
    // Drop existing GPU meshes + textures + materials.
    for (auto& gm : gpuMeshes_) {
        if (gm.positionBuffer) gm.positionBuffer->release();
        if (gm.normalBuffer)   gm.normalBuffer->release();
        if (gm.uvBuffer)       gm.uvBuffer->release();
        if (gm.uv1Buffer)      gm.uv1Buffer->release();
        if (gm.indexBuffer)    gm.indexBuffer->release();
    }
    gpuMeshes_.clear();
    gpuMeshes_.reserve(scene.meshes.size());
    releaseTexturePool();

    projection_     = scene.camera.projection;
    view_           = scene.camera.view;
    // Camera world position = 4th column of the camera-to-world matrix.
    cameraWorldPos_ = glm::vec3(scene.camera.world[3]);

    // ---- Upload textures from scene.textures ----
    texturePool_.reserve(scene.textures.size());
    for (size_t i = 0; i < scene.textures.size(); ++i) {
        const auto& t = scene.textures[i];
        MTL::Texture* mt = (t.width > 0 && t.height > 0 && !t.rgba.empty())
            ? createTextureFromRgba(t.rgba.data(), t.width, t.height,
                                      t.isSRGB, t.name.c_str())
            : nullptr;
        texturePool_.push_back(mt);
        if (mt) {
            std::printf("[MetalRenderer] Tex %zu '%s' %dx%d %s\n",
                         i, t.name.c_str(), t.width, t.height,
                         t.isSRGB ? "sRGB" : "linear");
        }
    }

    // ---- Resolve materials ----
    auto resolveTex = [&](int texIdx, MTL::Texture* fallback) -> MTL::Texture* {
        if (texIdx < 0
            || texIdx >= static_cast<int>(texturePool_.size())) return fallback;
        return texturePool_[texIdx] ? texturePool_[texIdx] : fallback;
    };
    gpuMaterials_.reserve(scene.materials.size());
    for (const auto& m : scene.materials) {
        GpuMaterial gm;
        gm.baseColorTex   = resolveTex(m.baseColorTex,
                                         defaultWhite_);
        gm.mrTex          = resolveTex(m.metallicRoughnessTex,
                                         defaultLinear_);
        gm.emissiveTex    = resolveTex(m.emissiveTex,
                                         defaultBlack_);
        gm.baseColorFactor = m.baseColorFactor;
        gm.metallicFactor  = m.metallicFactor;
        gm.roughnessFactor = m.roughnessFactor;
        gm.emissiveFactor  = m.emissiveFactor;
        gpuMaterials_.push_back(gm);
    }
    std::printf("[MetalRenderer] Materials loaded: %zu (textures pool: %zu)\n",
                 gpuMaterials_.size(), texturePool_.size());

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
        gm.materialIdx = m.materialIdx;

        const size_t vbBytes = m.positions.size() * sizeof(glm::vec3);
        gm.positionBuffer = device_->newBuffer(
            m.positions.data(), vbBytes, MTL::ResourceStorageModeShared);

        if (!m.normals.empty()) {
            const size_t nbBytes = m.normals.size() * sizeof(glm::vec3);
            gm.normalBuffer = device_->newBuffer(
                m.normals.data(), nbBytes, MTL::ResourceStorageModeShared);
        } else {
            std::vector<glm::vec3> fakeNormals(m.positions.size(),
                                                glm::vec3(0, 0, 1));
            gm.normalBuffer = device_->newBuffer(
                fakeNormals.data(), fakeNormals.size() * sizeof(glm::vec3),
                MTL::ResourceStorageModeShared);
        }

        // UV buffer (vec2). Synthesize (0,0) per vertex if no UVs were
        // exported — keeps the vertex layout uniform so the shader always
        // has something at attribute(2).
        if (!m.uvs.empty()) {
            const size_t uvBytes = m.uvs.size() * sizeof(glm::vec2);
            gm.uvBuffer = device_->newBuffer(
                m.uvs.data(), uvBytes, MTL::ResourceStorageModeShared);
        } else {
            std::vector<glm::vec2> fakeUVs(m.positions.size(),
                                            glm::vec2(0.0f, 0.0f));
            gm.uvBuffer = device_->newBuffer(
                fakeUVs.data(), fakeUVs.size() * sizeof(glm::vec2),
                MTL::ResourceStorageModeShared);
        }

        // UV1 buffer — dedicated to Syphon overlay. When the export carries
        // a TEXCOORD_1 we allocate a separate buffer for it. Otherwise we
        // share the SAME underlying buffer with UV0 so the VS doesn't pay
        // double bandwidth reading two identical attributes. Sharing is
        // safe because the buffer is read-only and Metal allows the same
        // MTLBuffer to be bound at multiple vertex slots.
        if (!m.uvs1.empty()) {
            const size_t uvBytes = m.uvs1.size() * sizeof(glm::vec2);
            gm.uv1Buffer = device_->newBuffer(
                m.uvs1.data(), uvBytes, MTL::ResourceStorageModeShared);
        } else {
            // Share with uvBuffer. Retain so the destructor's per-buffer
            // release stays balanced (both pointers now reference-count it).
            gm.uv1Buffer = gm.uvBuffer;
            if (gm.uv1Buffer) gm.uv1Buffer->retain();
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
    const std::vector<const BeamLayer*>& spots,
    const std::vector<const DirectionalLightLayer*>& dirs,
    const glm::vec3& ambientColor,
    MTL::Texture* syphonTex,
    float          syphonMix,
    const glm::vec3& syphonTint,
    bool           syphonFlipY,
    bool           showHeatmap,
    int            heatmapMetric,
    int            heatmapUV,
    float          projectorOnFlatMix,
    float          projectorFlatnessThreshold,
    const std::vector<VirtualSpot>& virtualSpots)
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
    // In lights-only mode the clear is fully transparent so unlit areas
    // composite cleanly. Otherwise dark slate so the relief reads.
    if (layer.emitLightsOnly) {
        colorAttachment->setClearColor(MTL::ClearColor(0.0, 0.0, 0.0, 0.0));
    } else {
        colorAttachment->setClearColor(MTL::ClearColor(0.04, 0.05, 0.07, 1.0));
    }

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

    const double t = ctx.elapsedSeconds;

    // Pack spot lights. Each BeamLayer is a rig that expands to N fixtures;
    // each fixture has its own world position, color, and phase-shifted LFO
    // evaluation. Total spots clamped to kMaxSpots.
    GpuSpot spotsPacked[kMaxSpots]{};
    int spotCount = 0;
    for (const BeamLayer* s : spots) {
        if (!s || spotCount >= kMaxSpots) break;
        glm::vec3 baseFwd = s->followCamera
            ? glm::normalize(ctx.cameraForward)
            : glm::vec3(0.0f, 1.0f, 0.0f);
        auto positions = s->fixturePositions(ctx);
        int  N = static_cast<int>(positions.size());

        float innerRad = s->innerDeg * 3.14159265f / 180.0f;
        float outerRad = s->outerDeg * 3.14159265f / 180.0f;
        if (outerRad < innerRad) outerRad = innerRad;
        float innerCos = std::cos(innerRad);
        float outerCos = std::cos(outerRad);

        const ModulatorBank* mods = ctx.scene ? &ctx.scene->modulators : nullptr;
        for (int i = 0; i < N && spotCount < kMaxSpots; ++i, ++spotCount) {
            glm::vec3 origin = positions[i];
            glm::vec3 dir    = s->directionAtTimeForFixture(t, i, N, baseFwd, mods);
            float     inten  = s->intensityAtTimeForFixture(t, i, N, mods)
                               * s->opacity;
            glm::vec3 col    = s->colorForFixture(i);

            spotsPacked[spotCount].posIntensity =
                glm::vec4(origin, inten);
            spotsPacked[spotCount].dirRange     =
                glm::vec4(dir, s->range);
            spotsPacked[spotCount].colorInner   =
                glm::vec4(col, innerCos);
            spotsPacked[spotCount].paramsOuter  =
                glm::vec4(outerCos, 0.0f, 0.0f, 0.0f);
        }
    }

    // Append LightClonerLayer-expanded virtual spots into the same array,
    // capped at kMaxSpots. Each VirtualSpot is already fully resolved
    // (position + direction + color + cone cosines) by the cloner's
    // expandSpots() call site upstream.
    for (const auto& vs : virtualSpots) {
        if (spotCount >= kMaxSpots) break;
        spotsPacked[spotCount].posIntensity = glm::vec4(vs.worldPos,  vs.intensity);
        spotsPacked[spotCount].dirRange     = glm::vec4(vs.direction, vs.range);
        spotsPacked[spotCount].colorInner   = glm::vec4(vs.color,     vs.innerCos);
        spotsPacked[spotCount].paramsOuter  = glm::vec4(vs.outerCos, 0.0f, 0.0f, 0.0f);
        ++spotCount;
    }

    // Pack directional lights (pan/tilt + intensity LFOs).
    int dirCount = std::min(static_cast<int>(dirs.size()), kMaxDirs);
    GpuDir dirsPacked[kMaxDirs]{};
    for (int i = 0; i < dirCount; ++i) {
        const DirectionalLightLayer* d = dirs[i];
        if (!d) continue;
        glm::vec3 dvec = d->directionAtTime(t);
        float     inten = d->intensityAtTime(t) * d->opacity;
        dirsPacked[i].dirIntensity = glm::vec4(dvec, inten);
        dirsPacked[i].color        = glm::vec4(d->color, 0.0f);
    }

    for (const auto& gm : gpuMeshes_) {
        // Resolve the material for this mesh (or the default).
        const GpuMaterial& mat =
            (gm.materialIdx >= 0
             && gm.materialIdx < static_cast<int>(gpuMaterials_.size()))
            ? gpuMaterials_[gm.materialIdx]
            : defaultMaterial_;

        Uniforms u{};
        u.projection         = ctx.projection;
        u.view               = ctx.view;
        u.model              = gm.transform;
        u.cameraWorldPos     = glm::vec4(ctx.cameraWorldPos, 1.0f);
        u.baseColorRoughness = glm::vec4(layer.baseColor, layer.roughness);
        u.metallicCounts     = glm::vec4(layer.metallic,
                                          0.0f,
                                          static_cast<float>(spotCount),
                                          static_cast<float>(dirCount));
        u.ambientColor       = glm::vec4(ambientColor, 0.0f);
        u.modeFlags          = glm::vec4(layer.emitLightsOnly ? 1.0f : 0.0f,
                                          showHeatmap         ? 1.0f : 0.0f,
                                          static_cast<float>(heatmapMetric),
                                          static_cast<float>(heatmapUV));
        u.matBaseColor       = mat.baseColorFactor;
        u.matEmissive        = glm::vec4(mat.emissiveFactor, 0.0f);
        u.matMR              = glm::vec4(mat.metallicFactor,
                                          mat.roughnessFactor,
                                          0.0f, 0.0f);
        // Mesh-effect uniforms. ModulatorBank-driven contributions are
        // added on top of the operator's static values.
        const ModulatorBank* mods = ctx.scene ? &ctx.scene->modulators : nullptr;
        float dispEff = layer.displaceAmount;
        float twistEff = layer.twistAmount;
        if (mods) {
            dispEff  += mods->eval(layer.displaceModSlot, t) * layer.displaceModDepth;
            twistEff += mods->eval(layer.twistModSlot,    t) * layer.twistModDepth;
        }
        u.fx = glm::vec4(dispEff, layer.displaceScale, twistEff, 0.0f);
        u.syphonMixTint = glm::vec4(syphonMix, syphonTint);
        u.syphonParams  = glm::vec4(projectorOnFlatMix,
                                     projectorFlatnessThreshold,
                                     syphonFlipY ? 1.0f : 0.0f,
                                     0.0f);
        std::memcpy(u.dirs,  dirsPacked,  sizeof(GpuDir)  * kMaxDirs);
        std::memcpy(u.spots, spotsPacked, sizeof(GpuSpot) * kMaxSpots);

        enc->setVertexBuffer(gm.positionBuffer, 0, 0);
        enc->setVertexBuffer(gm.normalBuffer,   0, 2);
        enc->setVertexBuffer(gm.uvBuffer,       0, 3);
        enc->setVertexBuffer(gm.uv1Buffer,      0, 4);
        enc->setVertexBytes(&u, sizeof(u), 1);
        enc->setFragmentBytes(&u, sizeof(u), 0);

        // Bind material textures + sampler.
        enc->setFragmentTexture(mat.baseColorTex
            ? mat.baseColorTex  : defaultWhite_,  0);
        enc->setFragmentTexture(mat.mrTex
            ? mat.mrTex         : defaultLinear_, 1);
        enc->setFragmentTexture(mat.emissiveTex
            ? mat.emissiveTex   : defaultBlack_,  2);
        enc->setFragmentTexture(syphonTex
            ? syphonTex         : defaultBlack_,  3);
        enc->setFragmentSamplerState(linearSampler_, 0);

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

bool MetalRenderer::reloadMesh(size_t meshIdx, const MeshData& m) {
    if (meshIdx >= gpuMeshes_.size()) return false;
    if (m.positions.empty() || m.indices.empty()) return false;

    GpuMesh& gm = gpuMeshes_[meshIdx];

    // Release the old buffers; we'll allocate fresh ones at the new sizes.
    // xatlas typically grows the vertex count (splits at seams), so the
    // old buffers are too small anyway. Releasing here is safe because
    // Metal buffers are reference-counted and any in-flight command buffer
    // already holds its own retain on the buffer (Metal::CommandBuffer
    // captures the resources on encode).
    if (gm.positionBuffer) { gm.positionBuffer->release(); gm.positionBuffer = nullptr; }
    if (gm.normalBuffer)   { gm.normalBuffer->release();   gm.normalBuffer   = nullptr; }
    if (gm.uvBuffer)       { gm.uvBuffer->release();       gm.uvBuffer       = nullptr; }
    if (gm.uv1Buffer)      { gm.uv1Buffer->release();      gm.uv1Buffer      = nullptr; }
    if (gm.indexBuffer)    { gm.indexBuffer->release();    gm.indexBuffer    = nullptr; }

    const size_t vbBytes = m.positions.size() * sizeof(glm::vec3);
    gm.positionBuffer = device_->newBuffer(
        m.positions.data(), vbBytes, MTL::ResourceStorageModeShared);

    if (!m.normals.empty()) {
        const size_t nbBytes = m.normals.size() * sizeof(glm::vec3);
        gm.normalBuffer = device_->newBuffer(
            m.normals.data(), nbBytes, MTL::ResourceStorageModeShared);
    } else {
        std::vector<glm::vec3> fake(m.positions.size(), glm::vec3(0, 0, 1));
        gm.normalBuffer = device_->newBuffer(
            fake.data(), fake.size() * sizeof(glm::vec3),
            MTL::ResourceStorageModeShared);
    }

    if (!m.uvs.empty()) {
        const size_t uvBytes = m.uvs.size() * sizeof(glm::vec2);
        gm.uvBuffer = device_->newBuffer(
            m.uvs.data(), uvBytes, MTL::ResourceStorageModeShared);
    } else {
        std::vector<glm::vec2> fake(m.positions.size(), glm::vec2(0.0f));
        gm.uvBuffer = device_->newBuffer(
            fake.data(), fake.size() * sizeof(glm::vec2),
            MTL::ResourceStorageModeShared);
    }

    if (!m.uvs1.empty()) {
        const size_t uvBytes = m.uvs1.size() * sizeof(glm::vec2);
        gm.uv1Buffer = device_->newBuffer(
            m.uvs1.data(), uvBytes, MTL::ResourceStorageModeShared);
    } else {
        // Share uv0 buffer when no atlas — see loadScene rationale.
        gm.uv1Buffer = gm.uvBuffer;
        if (gm.uv1Buffer) gm.uv1Buffer->retain();
    }

    const size_t ibBytes = m.indices.size() * sizeof(uint32_t);
    gm.indexBuffer = device_->newBuffer(
        m.indices.data(), ibBytes, MTL::ResourceStorageModeShared);
    gm.indexCount = static_cast<uint32_t>(m.indices.size());

    std::printf("[MetalRenderer] Hot-reloaded mesh '%s': %zu verts, %u indices "
                 "(uv1: %s)\n",
                 gm.name.c_str(), m.positions.size(), gm.indexCount,
                 m.uvs1.empty() ? "mirrors uv0" : "atlas");
    return true;
}

// =============================================================================
// Effect-layer helpers (added 2026-05 alongside the 11 agent-designed
// effects under engine/effects/).
// =============================================================================

namespace {
// Lazy storage for the post-FX ping-pong scratch + sampler. Keeping them
// in an anonymous namespace local to this TU means we don't need to
// expand the MetalRenderer's private state to support the helpers.
struct PostFxState {
    MTL::Texture*      scratch    = nullptr;
    int                width      = 0;
    int                height     = 0;
    MTL::PixelFormat   format     = MTL::PixelFormatBGRA8Unorm;
    MTL::SamplerState* sampler    = nullptr;
};
static PostFxState gPostFx;

MTL::Texture* ensurePingPongScratch(MTL::Device* dev, MTL::Texture* ref) {
    int w = static_cast<int>(ref->width());
    int h = static_cast<int>(ref->height());
    MTL::PixelFormat fmt = ref->pixelFormat();
    if (gPostFx.scratch && gPostFx.width == w && gPostFx.height == h
        && gPostFx.format == fmt) return gPostFx.scratch;
    if (gPostFx.scratch) gPostFx.scratch->release();
    MTL::TextureDescriptor* td = MTL::TextureDescriptor::texture2DDescriptor(
        fmt, w, h, false);
    td->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
    td->setStorageMode(MTL::StorageModePrivate);
    gPostFx.scratch = dev->newTexture(td);
    gPostFx.width   = w;
    gPostFx.height  = h;
    gPostFx.format  = fmt;
    return gPostFx.scratch;
}
} // anonymous namespace

MTL::Texture* MetalRenderer::acquirePingPongTarget(RenderContext& ctx) {
    if (!ctx.colorTarget) return nullptr;
    return ensurePingPongScratch(device_, ctx.colorTarget);
}

void MetalRenderer::swapPingPong(RenderContext& ctx) {
    // After an effect rendered into the partner, swap so subsequent
    // layers see that as colorTarget. We retain/release to avoid the
    // engine owning the lifetime confusion: gPostFx.scratch holds a
    // strong reference; ctx.colorTarget is just a non-owning pointer
    // managed by the offscreen texture pool in Workstation.
    if (!ctx.colorTarget || !gPostFx.scratch) return;
    // Swap pointers: whatever ctx.colorTarget used to point at becomes
    // the new ping-pong scratch; ctx.colorTarget now points at what
    // was the scratch (and just got written).
    MTL::Texture* oldColor = ctx.colorTarget;
    ctx.colorTarget = gPostFx.scratch;
    gPostFx.scratch = oldColor;
    // (Sizes and format are guaranteed identical by ensurePingPongScratch.)
}

MTL::SamplerState* MetalRenderer::postFxSampler() {
    if (gPostFx.sampler) return gPostFx.sampler;
    MTL::SamplerDescriptor* sd = MTL::SamplerDescriptor::alloc()->init();
    sd->setMinFilter(MTL::SamplerMinMagFilterLinear);
    sd->setMagFilter(MTL::SamplerMinMagFilterLinear);
    sd->setMipFilter(MTL::SamplerMipFilterNotMipmapped);
    sd->setSAddressMode(MTL::SamplerAddressModeClampToEdge);
    sd->setTAddressMode(MTL::SamplerAddressModeClampToEdge);
    gPostFx.sampler = device_->newSamplerState(sd);
    sd->release();
    return gPostFx.sampler;
}

void MetalRenderer::renderVolumetricBeams(RenderContext& ctx,
                                           const VolumetricUniforms& u)
{
    // Stub implementation — the volumetric beams effect is DEFAULT-OFF
    // per the operator constraint ("never show a light cone in air,
    // only the projection on the stage"). When you intentionally enable
    // it, this stub renders nothing; the full raymarch path is documented
    // in engine/effects/volumetric_beams/INTEGRATION.md and lives there
    // until somebody decides to break the projection-mapping rule.
    (void)ctx; (void)u;
}

} // namespace spacegen
