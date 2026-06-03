#include "MetalRenderer.h"

#include "../../core/Scene.h"
#include "../../core/Layer.h"
#include "../../core/StructureLayer.h"
#include "../../core/BeamLayer.h"
#include "../../core/DirectionalLightLayer.h"
#include "../../core/ModulatorBank.h"
#include "../../effects/light_cloner/LightClonerLayer.h"
#include "../../effects/area_light/AreaLightLayer.h"
#include "../../effects/mesh_deformation/MeshDeformationLayer.h"
#include "../../effects/procedural_material/ProceduralMaterialLayer.h"
#include "../../effects/mesh_fracture/MeshFractureLayer.h"

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
constexpr int kMaxAreas = 8;    // AreaLightLayer panels — see effects/area_light.
constexpr int kRendDeformOps = 16;  // MeshDeformationLayer chain — see deformers.metal.inc.
constexpr int kRendFfdSlots  = 4;
constexpr const char* kStructurePbrMSL = R"MSL(
#include <metal_stdlib>
using namespace metal;

constant float PI         = 3.14159265359;
constant int   MAX_SPOTS  = 64;
constant int   MAX_DIRS   = 4;
constant int   MAX_AREAS  = 8;

// ---- MeshDeformationLayer chain (mirrors deformers.metal.inc) ----
constant int OP_NONE       = 0;
constant int OP_CURL_NOISE = 1;
constant int OP_VORONOI    = 2;
constant int OP_TWIST      = 3;
constant int OP_BEND       = 4;
constant int OP_TAPER      = 5;
constant int OP_WAVE       = 6;
constant int OP_SPHERIFY   = 7;
constant int OP_FFD_LITE   = 8;
constant int MAX_DEFORM_OPS = 16;
constant int MAX_FFD_SLOTS  = 4;

struct GpuDeformOp {
    float4 header;   // .x type, .y intensity, .z time, .w ffd slot index (-1 unless FFD)
    float4 pA;
    float4 pB;
};

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

// AreaLightLayer: a finite-extent disc-equivalent panel that orbits the
// structure. Karis 2013 representative-point GGX + Lagarde 2014 closed-
// form Lambertian disc form-factor (see effects/area_light/INTEGRATION.md).
struct AreaLight {
    float4 posIntensity;   // .xyz position (world),     .w intensity
    float4 normRange;      // .xyz panel normal (unit),  .w linear range
    float4 colorRadius;    // .rgb color,                .a equivalent radius
};

struct Uniforms {
    float4x4  projection;
    float4x4  view;
    float4x4  model;
    float4    cameraWorldPos;
    float4    baseColorRoughness;   // .rgb operator tint, .a operator roughness multiplier
    float4    metallicCounts;       // .x metallic operator, .y areaCount, .z spotCount, .w dirCount
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
    // ---- Surface-effect MVP splice (Hologram / ProceduralMaterial /
    //      MeshDeformation / MeshFracture, all packed into 2 vec4s). Each
    //      effect's layer in the bus sets its slot; 0 = off, > 0 = active.
    //      Full agent MSL stays in engine/effects/*/INTEGRATION.md for
    //      future quality upgrades. ----
    float4    surfaceFx;            // .x holoOpacity, .y procMix, .z deformAmt, .w fractureAmt
    float4    surfaceFxParams;      // .x procScale, .y deformScale, .z fractureSeed, .w time
    // ---- MeshDeformationLayer chain (full agent dispatcher) ----
    float4    deformMeta;           // .x deformCount, .y maxDisplacement(m), .zw unused
    GpuDeformOp deformers[MAX_DEFORM_OPS];
    float4    ffdCorners[MAX_FFD_SLOTS * 8];   // 4 slots × 8 corner deltas
    // ---- ProceduralMaterialLayer (full 10-pattern dispatcher, triplanar) ----
    float4    procColorA;           // .rgb colour A, .a opacity
    float4    procColorB;           // .rgb colour B, .a opacity
    float4    procShape;            // .x scale, .y contrast, .z mix, .w pattern
    float4    procAnim;             // .xy drift, .z time mult, .w octaves
    // ---- MeshFractureLayer (4 modes: cracks/dissolve/explode/glitch) ----
    float4    fracMeta;             // .x mode(bits), .y amount, .z seed, .w time
    float4    fracCracks;           // .x density, .y width, .z jitter, .w darken
    float4    fracCrackGlow;        // .rgb glow colour, .w glow strength
    float4    fracDissolveA;        // .x scale, .y speed, .z radius, .w radialBias
    float4    fracDissolveB;        // .x edgeWidth, .y edgeGlow, .z useRadial, .w jitterSpeed
    float4    fracBurn;             // .xyz burn point, .w unused
    float4    fracDissolveColor;    // .rgb edge colour, .w unused
    float4    fracExplode;          // .x shardDensity, .y jitter, .z spin, .w strength
    float4    fracMeshCenter;       // .xyz world mesh centroid, .w explodeCap(m)
    float4    fracGlitch;           // .x freq, .y magnitude, .z speed, .w dropout
    DirLight  dirs[MAX_DIRS];
    SpotLight spots[MAX_SPOTS];
    AreaLight areas[MAX_AREAS];
};

// Hash-based pseudo-random scalar in [-1, 1] from a 3D position.
static float vsHash(float3 p) {
    float3 s = sin(p * float3(127.1, 311.7, 74.7));
    return fract(s.x + s.y + s.z) * 2.0 - 1.0;
}

// ============================================================
// MeshDeformationLayer dispatcher (inlined from deformers.metal.inc).
// Prefixed `df` to avoid collision with the sfx* / vsHash helpers.
// ============================================================
static float dfHash13(float3 p) {
    float n = dot(p, float3(127.1, 311.7, 74.7));
    return fract(sin(n) * 43758.5453123) * 2.0 - 1.0;
}
static float3 dfHash33(float3 p) {
    p = float3(dot(p, float3(127.1, 311.7, 74.7)),
                dot(p, float3(269.5, 183.3, 246.1)),
                dot(p, float3(113.5, 271.9, 124.6)));
    return fract(sin(p) * 43758.5453123) * 2.0 - 1.0;
}
static float3 dfFade5(float3 t) {
    return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
}
static float dfNoise3(float3 p) {
    float3 i = floor(p);
    float3 f = fract(p);
    float3 u = dfFade5(f);
    float n000 = dot(dfHash33(i + float3(0,0,0)), f - float3(0,0,0));
    float n100 = dot(dfHash33(i + float3(1,0,0)), f - float3(1,0,0));
    float n010 = dot(dfHash33(i + float3(0,1,0)), f - float3(0,1,0));
    float n110 = dot(dfHash33(i + float3(1,1,0)), f - float3(1,1,0));
    float n001 = dot(dfHash33(i + float3(0,0,1)), f - float3(0,0,1));
    float n101 = dot(dfHash33(i + float3(1,0,1)), f - float3(1,0,1));
    float n011 = dot(dfHash33(i + float3(0,1,1)), f - float3(0,1,1));
    float n111 = dot(dfHash33(i + float3(1,1,1)), f - float3(1,1,1));
    float nx00 = mix(n000, n100, u.x);
    float nx10 = mix(n010, n110, u.x);
    float nx01 = mix(n001, n101, u.x);
    float nx11 = mix(n011, n111, u.x);
    float nxy0 = mix(nx00, nx10, u.y);
    float nxy1 = mix(nx01, nx11, u.y);
    return mix(nxy0, nxy1, u.z);
}
static float3 dfNoiseField(float3 p) {
    return float3(dfNoise3(p + float3(0.0,  0.0,  0.0)),
                   dfNoise3(p + float3(31.4, 17.3, 11.0)),
                   dfNoise3(p + float3(91.0, 64.7, 47.2)));
}
static float3 dfRotateAxis(float3 v, float3 a, float theta) {
    float c = cos(theta), s = sin(theta);
    return v * c + cross(a, v) * s + a * dot(a, v) * (1.0 - c);
}
static float3 dfCurlOf(float3 p) {
    const float e = 1e-2;
    float3 dx = (dfNoiseField(p + float3(e,0,0)) - dfNoiseField(p - float3(e,0,0))) / (2.0*e);
    float3 dy = (dfNoiseField(p + float3(0,e,0)) - dfNoiseField(p - float3(0,e,0))) / (2.0*e);
    float3 dz = (dfNoiseField(p + float3(0,0,e)) - dfNoiseField(p - float3(0,0,e))) / (2.0*e);
    return float3(dy.z - dz.y, dz.x - dx.z, dx.y - dy.x);
}
static float3 dfOpCurlNoise(float3 pos, GpuDeformOp op) {
    float scale = op.pA.x, amount = op.pA.y, speed = op.pA.z;
    bool timeOn = op.pA.w > 0.5;
    float t = op.header.z;
    float3 q = pos * scale;
    if (timeOn) q += t * speed * float3(1.0, 1.3, 0.7);
    return pos + dfCurlOf(q) * amount * op.header.y;
}
static float3 dfOpVoronoi(float3 pos, GpuDeformOp op) {
    float scale = max(op.pA.x, 1e-3), shatter = op.pA.y;
    float jitter = clamp(op.pA.z, 0.0, 1.0);
    float3 q = pos / scale, i = floor(q), f = q - i;
    float bestDist = 1e9; float3 bestCell = float3(0.0);
    for (int dz = -1; dz <= 1; ++dz)
    for (int dy = -1; dy <= 1; ++dy)
    for (int dx = -1; dx <= 1; ++dx) {
        float3 cell = float3(float(dx), float(dy), float(dz));
        float3 r = (dfHash33(i + cell) * 0.5 + 0.5) * jitter;
        float3 diff = (cell + r) - f;
        float d = dot(diff, diff);
        if (d < bestDist) { bestDist = d; bestCell = cell + r; }
    }
    float3 centerWorld = (i + bestCell) * scale;
    return pos + (centerWorld - pos) * shatter * op.header.y;
}
static float3 dfOpTwist(float3 pos, GpuDeformOp op) {
    float3 axis = normalize(op.pA.xyz + float3(1e-6));
    float amount = op.pA.w * op.header.y;
    float3 center = op.pB.xyz;
    float3 q = pos - center;
    return dfRotateAxis(q, axis, dot(q, axis) * amount) + center;
}
static float3 dfOpBend(float3 pos, GpuDeformOp op) {
    float3 axis = normalize(op.pA.xyz + float3(1e-6));
    float angle = op.pA.w * op.header.y;
    float3 bendDir = normalize(op.pB.xyz + float3(1e-6, 0.0, 0.0));
    float3 rotAxis = normalize(cross(axis, bendDir) + float3(0.0, 1e-6, 0.0));
    return dfRotateAxis(pos, rotAxis, dot(pos, bendDir) * angle);
}
static float3 dfOpTaper(float3 pos, GpuDeformOp op) {
    float3 axis = normalize(op.pA.xyz + float3(1e-6));
    float amount = op.pA.w * op.header.y;
    float3 center = op.pB.xyz;
    bool doClamp = op.pB.w > 0.5;
    float3 q = pos - center;
    float h = dot(q, axis);
    float s = 1.0 + h * amount;
    if (doClamp) s = max(s, 0.0);
    float3 axial = axis * h;
    return center + axial + (q - axial) * s;
}
static float3 dfOpWave(float3 pos, float3 vertexNormal, GpuDeformOp op) {
    float3 dir = normalize(op.pA.xyz + float3(1e-6));
    float freq = op.pA.w, speed = op.pB.x, amp = op.pB.y * op.header.y;
    int mode = int(op.pB.z + 0.5);
    float t = op.header.z;
    float s = sin(dot(pos, dir) * freq - t * speed) * amp;
    float3 along = (mode == 1) ? float3(0.0, 0.0, 1.0)
                  : (mode == 2) ? dir
                  : normalize(vertexNormal + float3(1e-6, 0.0, 0.0));
    return pos + along * s;
}
static float3 dfOpSpherify(float3 pos, GpuDeformOp op) {
    float3 center = op.pA.xyz;
    float radius = op.pA.w, strength = op.pB.x * op.header.y;
    float3 d = pos - center;
    float len = length(d) + 1e-6;
    return mix(pos, center + (d / len) * radius, clamp(strength, 0.0, 1.0));
}
static float3 dfOpFfdLite(float3 pos, GpuDeformOp op,
                           constant float4* ffdCornersBase) {
    float3 bmin = op.pA.xyz, bmax = op.pB.xyz;
    int slot = int(op.header.w + 0.5);
    if (slot < 0) return pos;
    float3 size = max(bmax - bmin, float3(1e-4));
    float3 u = clamp((pos - bmin) / size, 0.0, 1.0);
    constant float4* sb = ffdCornersBase + slot * 8;
    float3 b000 = float3(bmin.x, bmin.y, bmin.z) + sb[0].xyz;
    float3 b100 = float3(bmax.x, bmin.y, bmin.z) + sb[1].xyz;
    float3 b010 = float3(bmin.x, bmax.y, bmin.z) + sb[2].xyz;
    float3 b110 = float3(bmax.x, bmax.y, bmin.z) + sb[3].xyz;
    float3 b001 = float3(bmin.x, bmin.y, bmax.z) + sb[4].xyz;
    float3 b101 = float3(bmax.x, bmin.y, bmax.z) + sb[5].xyz;
    float3 b011 = float3(bmin.x, bmax.y, bmax.z) + sb[6].xyz;
    float3 b111 = float3(bmax.x, bmax.y, bmax.z) + sb[7].xyz;
    float3 dx00 = mix(b000, b100, u.x);
    float3 dx10 = mix(b010, b110, u.x);
    float3 dx01 = mix(b001, b101, u.x);
    float3 dx11 = mix(b011, b111, u.x);
    float3 dxy0 = mix(dx00, dx10, u.y);
    float3 dxy1 = mix(dx01, dx11, u.y);
    return mix(pos, mix(dxy0, dxy1, u.z), op.header.y);
}
static float3 dfApplyChain(float3 pos, float3 vertexNormal,
                            constant GpuDeformOp* ops, int opCount,
                            constant float4* ffdCornersBase) {
    float3 p = pos;
    for (int i = 0; i < opCount && i < MAX_DEFORM_OPS; ++i) {
        GpuDeformOp op = ops[i];
        int type = int(op.header.x + 0.5);
        if (type == OP_NONE || op.header.y <= 0.0) continue;
        if      (type == OP_CURL_NOISE) p = dfOpCurlNoise(p, op);
        else if (type == OP_VORONOI)    p = dfOpVoronoi(p, op);
        else if (type == OP_TWIST)      p = dfOpTwist(p, op);
        else if (type == OP_BEND)       p = dfOpBend(p, op);
        else if (type == OP_TAPER)      p = dfOpTaper(p, op);
        else if (type == OP_WAVE)       p = dfOpWave(p, vertexNormal, op);
        else if (type == OP_SPHERIFY)   p = dfOpSpherify(p, op);
        else if (type == OP_FFD_LITE)   p = dfOpFfdLite(p, op, ffdCornersBase);
    }
    return p;
}

// ============================================================
// MeshFractureLayer — vertex-stage displacement (Explode + Glitch).
// Operates in WORLD space (after u.model). Bounded by explodeCap so
// shards stay near the physical structure (projection-mapping rule).
// Mode bits: 1=Cracks 2=Dissolve 4=Explode 8=Glitch.
// ============================================================
static float3 fracVertexDisplace(float3 wp, constant Uniforms& u) {
    uint  mode   = uint(u.fracMeta.x + 0.5);
    float amount = u.fracMeta.y;
    if (amount < 1e-3) return wp;

    if ((mode & 4u) != 0u) {   // Explode: per-shard outward push + spin
        float density = max(u.fracExplode.x, 0.3);
        float3 cellId = floor(wp * density);
        float3 shardCentroid = (cellId + 0.5) / density;
        float3 jit = dfHash33(cellId) * u.fracExplode.y;   // [-1,1]·jitter
        float3 meshC = u.fracMeshCenter.xyz;
        float3 dir = normalize(shardCentroid - meshC + jit + float3(1e-5));
        float cap  = max(u.fracMeshCenter.w, 0.0);
        float push = min(u.fracExplode.w * amount, cap);   // hard cap (m)
        wp += dir * push;
        float ang = u.fracExplode.z * amount * 6.2831 * dfHash13(cellId + 7.0);
        float3 rel = wp - shardCentroid;
        float c = cos(ang), s = sin(ang);
        rel = float3(c * rel.x - s * rel.y, s * rel.x + c * rel.y, rel.z);
        wp = shardCentroid + rel;
    }
    if ((mode & 8u) != 0u) {   // Glitch: lateral band tear (capped)
        float band = floor(wp.y * u.fracGlitch.x);
        float roll = floor(u.fracMeta.w * u.fracGlitch.z);
        float sh = dfHash13(float3(band, roll, 0.0))
                 * min(u.fracGlitch.y, 0.3) * amount;
        wp.x += sh;
    }
    return wp;
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

    // ---- MeshDeformationLayer: full agent deformer chain ----
    // When the operator has armed ops, run the real dispatcher (bend,
    // twist, taper, curl-noise, wave, spherify, FFD). Otherwise fall
    // back to the single-wave MVP gated on surfaceFx.z. Displacement is
    // clamped to deformMeta.y meters so layered octaves can never detach
    // the mesh from the physical structure it's mapped onto.
    int deformCount = int(u.deformMeta.x + 0.5);
    if (deformCount > 0) {
        float3 deformed = dfApplyChain(pos, in.normal,
                                        &u.deformers[0], deformCount,
                                        &u.ffdCorners[0]);
        float maxDisp = u.deformMeta.y > 1e-4 ? u.deformMeta.y : 1.5;
        float3 delta = deformed - pos;
        float len = length(delta);
        if (len > maxDisp) delta *= (maxDisp / len);
        pos += delta;
    } else {
        float deformAmount = u.surfaceFx.z;
        float deformScale  = u.surfaceFxParams.y;
        float sfxTime      = u.surfaceFxParams.w;
        if (deformAmount > 1e-4) {
            float w = sin(pos.x * deformScale + sfxTime * 2.0)
                    * cos(pos.z * deformScale + sfxTime * 1.3);
            pos += in.normal * deformAmount * w;
        }
    }

    float4 world = u.model * float4(pos, 1.0);
    // MeshFractureLayer vertex displacement (Explode + Glitch) in world space.
    float3 wp = fracVertexDisplace(world.xyz, u);
    out.position = u.projection * u.view * float4(wp, 1.0);
    out.worldPos = wp;
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

// ---- Surface-effect MVP helpers (MSL) ----
// Minimum-viable visible implementations of the agent-designed surface
// effects (Hologram / ProceduralMaterial / MeshDeformation / MeshFracture).
// Activated when the respective surfaceFx slot > 0. Full agent MSL with
// 10-pattern procedural / 7-sub-effect hologram / 8-deformer chain / 4-mode
// fracture stays in engine/effects/*/INTEGRATION.md for incremental quality
// upgrades — this is the "all of them at least show something" landing.

// Simple hash + value-noise (Inigo Quilez canon).
static float sfxHash21(float2 p) {
    return fract(sin(dot(p, float2(127.1, 311.7))) * 43758.5453);
}
static float sfxHash31(float3 p) {
    return fract(sin(dot(p, float3(127.1, 311.7, 74.7))) * 43758.5453);
}
static float sfxValueNoise(float2 p) {
    float2 i = floor(p);
    float2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = sfxHash21(i + float2(0,0));
    float b = sfxHash21(i + float2(1,0));
    float c = sfxHash21(i + float2(0,1));
    float d = sfxHash21(i + float2(1,1));
    return mix(mix(a,b,f.x), mix(c,d,f.x), f.y);
}
// 2D voronoi (F1 cell distance). For procedural-material splice + cracks.
static float sfxVoronoi(float2 p, thread float2& cellCenter) {
    float2 i = floor(p);
    float2 f = fract(p);
    float best = 1e9;
    cellCenter = i;
    for (int yy = -1; yy <= 1; ++yy)
    for (int xx = -1; xx <= 1; ++xx) {
        float2 g = float2(xx, yy);
        float2 o = float2(sfxHash21(i + g),
                          sfxHash21(i + g + 17.0));
        float2 d = g + o - f;
        float dist = dot(d, d);
        if (dist < best) { best = dist; cellCenter = i + g; }
    }
    return sqrt(best);
}

// Hologram overlay — Fresnel rim + scanlines + hue shift. Output is the
// final fragment colour after applying opacity-controlled blend over PBR.
static float3 sfxHologramOverlay(float3 baseLit, float3 N, float3 V,
                                  float3 worldPos, float opacity,
                                  float time) {
    if (opacity < 1e-3) return baseLit;
    // Fresnel rim (1 - N·V)^k.
    float fres = pow(1.0 - max(dot(N, V), 0.0), 3.0);
    float3 rimCol = float3(0.20, 0.85, 1.00) * fres * 2.5;
    // Scanlines: horizontal stripes on screen-Y, animated. We use
    // worldPos.z as a proxy here (screen Y not available without an
    // extra varying — would be cleaner but overkill for MVP).
    float scan = 0.5 + 0.5 * sin(worldPos.z * 80.0 + time * 4.0);
    float3 scanCol = baseLit * (0.7 + 0.3 * scan);
    // Combine.
    float3 holo = scanCol + rimCol;
    return mix(baseLit, holo, opacity);
}

// Procedural material — voronoi-cell colour replacement. Strength `mix01`
// fades in the procedural look; `scale` controls cell size.
static float3 sfxProceduralBase(float3 baseColor, float3 worldPos,
                                 float mix01, float scale, float time) {
    if (mix01 < 1e-3) return baseColor;
    float2 cell;
    float d = sfxVoronoi(worldPos.xy * scale + time * 0.3, cell);
    // Per-cell hue from hash.
    float h = sfxHash21(cell);
    float3 cellCol = float3(0.5 + 0.5 * cos(6.2831 * h + float3(0.0, 2.0, 4.0)));
    // Subtle border darkening — centers stay full color, only the edges
    // (where d is small) drop to 55% so cell shapes read without blacking
    // out the whole material. Previous code multiplied by `edge` directly,
    // which made centers (edge ≈ 0) pitch-black.
    float edge = smoothstep(0.30, 0.45, d);
    cellCol *= mix(0.55, 1.0, edge);
    return mix(baseColor, cellCol, mix01);
}

// Fracture — emit thin crack lines and (at higher amounts) discard cells.
// `amount` is already scaled by StructureLayer (opacity * 0.5) so the
// inspector slider 0..1 lands here as 0..0.5 → cracks only, no discard.
// The whole-cell discard tier kicks in above 0.8 — operator has to
// intentionally amplify (via modulator bank or layer stacking) to start
// shattering pieces of the structure.
static bool sfxFractureDiscardOrTint(float3 worldPos, float amount,
                                      float seed,
                                      thread float3& tintOut) {
    if (amount < 1e-3) { tintOut = float3(0.0); return false; }
    float2 cell;
    float d = sfxVoronoi(worldPos.xy * 1.5 + seed, cell);
    float crackBand = 1.0 - smoothstep(0.0, 0.05, d);
    tintOut = float3(1.0, 0.35, 0.10) * crackBand * amount * 2.0;
    // Hard discard for whole cells only above 0.8. Below that, just cracks.
    if (amount <= 0.8) return false;
    float cellAlive = sfxHash21(cell);
    return cellAlive < (amount - 0.8) * 3.0;
}

// ============================================================
// ProceduralMaterialLayer dispatcher (inlined from patterns.metal.inc).
// All helpers are `proc`-prefixed — no collision with sfx*/df*/vs*.
// Sampled triplanar in world space so the pattern paints the physical
// structure regardless of UV seams (projection-mapping invariant).
// ============================================================
static float procHash21(float2 p) {
    return fract(sin(dot(p, float2(127.1, 311.7))) * 43758.5453);
}
static float2 procHash22(float2 p) {
    float n = sin(dot(p, float2(127.1, 311.7)));
    return fract(float2(262144.0, 32768.0) * n);
}
static float3 procHash33(float3 p) {
    p = float3(dot(p, float3(127.1, 311.7,  74.7)),
               dot(p, float3(269.5, 183.3, 246.1)),
               dot(p, float3(113.5, 271.9, 124.6)));
    return fract(sin(p) * 43758.5453);
}
static float procValueNoise2(float2 p) {
    float2 i = floor(p);
    float2 f = fract(p);
    float2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    float a = procHash21(i + float2(0.0, 0.0));
    float b = procHash21(i + float2(1.0, 0.0));
    float c = procHash21(i + float2(0.0, 1.0));
    float d = procHash21(i + float2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}
static float procValueNoise3(float3 p) {
    float3 i = floor(p);
    float3 f = fract(p);
    float3 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    float a = procHash33(i + float3(0,0,0)).x;
    float b = procHash33(i + float3(1,0,0)).x;
    float c = procHash33(i + float3(0,1,0)).x;
    float d = procHash33(i + float3(1,1,0)).x;
    float e = procHash33(i + float3(0,0,1)).x;
    float g = procHash33(i + float3(1,0,1)).x;
    float h = procHash33(i + float3(0,1,1)).x;
    float k = procHash33(i + float3(1,1,1)).x;
    float ab = mix(a, b, u.x), cd = mix(c, d, u.x);
    float eg = mix(e, g, u.x), hk = mix(h, k, u.x);
    return mix(mix(ab, cd, u.y), mix(eg, hk, u.y), u.z);
}
static float procFbm2(float2 p, int octaves) {
    float v = 0.0, amp = 0.5;
    float2x2 rot = float2x2(0.8, 0.6, -0.6, 0.8);
    int n = clamp(octaves, 1, 8);
    for (int i = 0; i < n; ++i) { v += amp * procValueNoise2(p); p = rot * p * 2.0; amp *= 0.5; }
    return v;
}
static float procFbm3(float3 p, int octaves) {
    float v = 0.0, amp = 0.5;
    int n = clamp(octaves, 1, 8);
    for (int i = 0; i < n; ++i) { v += amp * procValueNoise3(p); p *= 2.0; amp *= 0.5; }
    return v;
}
static float3 procPalette(float t, float3 A, float3 B) {
    t = clamp(t, 0.0, 1.0);
    return mix(A, B, smoothstep(0.0, 1.0, t));
}
static float procApplyContrast(float t, float c) {
    float s = clamp(c, 0.0, 2.0);
    return clamp((t - 0.5) * s + 0.5, 0.0, 1.0);
}
static float3 procVoronoi(float2 p, float time, float contrast, float3 A, float3 B) {
    float2 i = floor(p), f = fract(p);
    float minD = 1e9;
    for (int y = -1; y <= 1; ++y)
    for (int x = -1; x <= 1; ++x) {
        float2 g = float2(float(x), float(y));
        float2 o = procHash22(i + g);
        o = 0.5 + 0.5 * sin(time + 6.2831 * o);
        float2 r = g + o - f;
        minD = min(minD, dot(r, r));
    }
    float F1 = sqrt(minD);
    if (contrast < 0.5) {
        float t = procApplyContrast(1.0 - F1, contrast * 2.0);
        return procPalette(t, A, B);
    } else {
        float edge = 0.04 + 0.16 * (1.0 - (contrast - 0.5) * 2.0);
        float aa   = fwidth(F1) * 1.5 + 1e-4;
        float k    = smoothstep(edge, edge + aa, F1);
        return procPalette(1.0 - k, A, B);
    }
}
static float3 procPerlinFbm(float2 p, float time, float contrast, int octaves, float3 A, float3 B) {
    float t = procFbm3(float3(p, time * 0.5), octaves);
    return procPalette(procApplyContrast(t, contrast), A, B);
}
static float3 procCurlNoise(float2 p, float time, float contrast, int octaves, float3 A, float3 B) {
    const float eps = 0.04;
    float psi_xp = procFbm3(float3(p + float2(eps, 0.0), time * 0.4), octaves);
    float psi_xm = procFbm3(float3(p - float2(eps, 0.0), time * 0.4), octaves);
    float psi_yp = procFbm3(float3(p + float2(0.0, eps), time * 0.4), octaves);
    float psi_ym = procFbm3(float3(p - float2(0.0, eps), time * 0.4), octaves);
    float2 v = float2((psi_yp - psi_ym) / (2.0 * eps), -(psi_xp - psi_xm) / (2.0 * eps));
    float t = saturate(length(v) * 0.5);
    return procPalette(procApplyContrast(t, contrast), A, B);
}
static float procHexDist(float2 p) {
    p.x *= 1.1547005;
    p.y += 0.5 * floor(fmod(p.x, 2.0));
    p = abs(fract(p) - 0.5);
    return abs(max(p.x * 1.5 + p.y, p.y * 2.0) - 1.0);
}
static float3 procHex(float2 p, float time, float contrast, float3 A, float3 B) {
    p += 0.05 * float2(cos(time * 0.7), sin(time * 0.5));
    float d  = procHexDist(p);
    float aa = fwidth(d) * 1.5 + 1e-4;
    float thresh = 0.04 + 0.10 * (2.0 - contrast);
    float k = smoothstep(thresh, thresh + aa, d);
    return procPalette(1.0 - k, A, B);
}
static float3 procChecker(float2 p, float time, float contrast, int octaves, float3 A, float3 B) {
    float c = 0.0, amp = 1.0, wsum = 0.0;
    float2 q = p * 3.1415926 + time * 0.5;
    int n = clamp(octaves, 1, 8);
    for (int i = 0; i < n; ++i) { c += amp * sign(sin(q.x) * sin(q.y)); wsum += amp; q *= 2.0; amp *= 0.5; }
    float t = (c / max(wsum, 1e-4)) * 0.5 + 0.5;
    return procPalette(procApplyContrast(t, contrast), A, B);
}
static float3 procRings(float2 p, float time, float contrast, float3 A, float3 B) {
    float d = length(p);
    float t = fract(d - time * 0.5);
    float w = clamp(0.5 - 0.45 * (contrast - 1.0), 0.05, 0.95);
    float band = smoothstep(0.5 - w * 0.5, 0.5, t) * (1.0 - smoothstep(0.5, 0.5 + w * 0.5, t));
    return procPalette(band, A, B);
}
static float3 procVoronoiShatter(float2 p, float time, float contrast, float3 A, float3 B) {
    float2 i = floor(p), f = fract(p);
    float F1 = 1e9, F2 = 1e9; float2 winnerCell = float2(0.0);
    for (int y = -1; y <= 1; ++y)
    for (int x = -1; x <= 1; ++x) {
        float2 g = float2(float(x), float(y));
        float2 o = procHash22(i + g);
        o = 0.5 + 0.5 * sin(time * 0.6 + 6.2831 * o);
        float2 r = g + o - f; float d2 = dot(r, r);
        if (d2 < F1) { F2 = F1; F1 = d2; winnerCell = i + g; }
        else if (d2 < F2) { F2 = d2; }
    }
    float edge = sqrt(F2) - sqrt(F1);
    float gapW = 0.02 + 0.18 * (2.0 - contrast);
    float gap  = smoothstep(0.0, gapW, edge);
    float3 inside = procPalette(procHash21(winnerCell), A, B);
    return mix(B * 0.25, inside, gap);
}
static float3 procWood(float2 p, float time, float contrast, int octaves, float3 A, float3 B) {
    float2 q = p;
    float r = sqrt(q.x * q.x * 0.1 + q.y * q.y);
    r += 0.55 * procFbm3(float3(q * 3.0, time * 0.2), octaves);
    float g = fract(r);
    float w = clamp(0.5 - 0.45 * (contrast - 1.0), 0.05, 0.95);
    g = smoothstep(0.5 - w * 0.5, 0.5 + w * 0.5, g);
    return procPalette(g, A, B);
}
static float3 procMarble(float2 p, float time, float contrast, int octaves, float3 A, float3 B) {
    float warp = procFbm3(float3(p, time * 0.15), octaves);
    float t = sin((p.x + 4.0 * warp) * 1.0) * 0.5 + 0.5;
    return procPalette(procApplyContrast(t, contrast), A, B);
}
static float3 procBrick(float2 p, float time, float contrast, float3 A, float3 B) {
    float2 q = float2(p.x * 0.5, p.y);
    float row = floor(q.y);
    float offset = fmod(row, 2.0) * 0.5;
    float u = fract(q.x + offset), v = fract(q.y);
    float mw = 0.04 + 0.06 * (2.0 - contrast);
    float mortarU = smoothstep(0.0, mw, u) * (1.0 - smoothstep(1.0 - mw, 1.0, u));
    float mortarV = smoothstep(0.0, mw, v) * (1.0 - smoothstep(1.0 - mw, 1.0, v));
    float brickMask = mortarU * mortarV;
    float col = floor(q.x + offset);
    float id  = procHash21(float2(col, row));
    id = fract(id + sin(time * 0.4 + id * 6.2831) * 0.05);
    float3 brick  = procPalette(id, A, B);
    float3 mortar = mix(A, B, 0.5) * 0.35;
    return mix(mortar, brick, brickMask);
}
static float3 procEvaluate2D(float2 uv, float elapsedSeconds,
                              float4 colA, float4 colB, float4 shape, float4 anim) {
    float scale = shape.x, contrast = shape.y;
    int pattern = int(shape.w + 0.5);
    int octaves = int(clamp(anim.w, 1.0, 8.0));
    float timeScale = anim.z;
    float3 A = colA.rgb, B = colB.rgb;
    float t = elapsedSeconds * timeScale;
    float2 sp = uv * scale + anim.xy * elapsedSeconds;
    switch (pattern) {
        case 0:  return procVoronoi       (sp, t, contrast, A, B);
        case 1:  return procPerlinFbm     (sp, t, contrast, octaves, A, B);
        case 2:  return procCurlNoise     (sp, t, contrast, octaves, A, B);
        case 3:  return procHex           (sp, t, contrast, A, B);
        case 4:  return procChecker       (sp, t, contrast, octaves, A, B);
        case 5:  return procRings         (sp, t, contrast, A, B);
        case 6:  return procVoronoiShatter(sp, t, contrast, A, B);
        case 7:  return procWood          (sp, t, contrast, octaves, A, B);
        case 8:  return procMarble        (sp, t, contrast, octaves, A, B);
        case 9:  return procBrick         (sp, t, contrast, A, B);
        default: return mix(A, B, 0.5);
    }
}
// World-space triplanar wrapper: blend three planar evaluations by the
// squared normal so the pattern sticks to the physical structure with no
// UV seam stretching — the #1 reason Notch materials read clean.
static float3 sfxProceduralTriplanar(float3 worldPos, float3 N, float elapsed,
                                      float4 colA, float4 colB,
                                      float4 shape, float4 anim) {
    float3 w = pow(abs(N), float3(4.0));
    w /= (w.x + w.y + w.z + 1e-5);
    float3 cx = procEvaluate2D(worldPos.yz, elapsed, colA, colB, shape, anim);
    float3 cy = procEvaluate2D(worldPos.zx, elapsed, colA, colB, shape, anim);
    float3 cz = procEvaluate2D(worldPos.xy, elapsed, colA, colB, shape, anim);
    return cx * w.x + cy * w.y + cz * w.z;
}

// ============================================================
// MeshFractureLayer — fragment-stage modes (Cracks/Dissolve/Glitch).
// Darkens baseColor and adds emissive; returns true to discard.
// Explode + Glitch-lateral happen in the vertex stage (fracVertexDisplace).
// ============================================================
static bool fracFragment(float3 wp, constant Uniforms& u,
                          thread float3& baseColor, thread float3& emissive) {
    uint  mode   = uint(u.fracMeta.x + 0.5);
    float amount = u.fracMeta.y;
    float seed   = u.fracMeta.z;
    float time   = u.fracMeta.w;
    if (mode == 0u || amount < 1e-3) return false;

    // CRACKS — Voronoi F1 edge band darkens + glows.
    if ((mode & 1u) != 0u) {
        float density = max(u.fracCracks.x, 0.2);
        float2 p = wp.xy * density + seed + time * u.fracDissolveB.w;
        float2 cell;
        float d = sfxVoronoi(p, cell);
        float width = max(u.fracCracks.y, 1e-3);
        float crackBand = 1.0 - smoothstep(0.0, width, d);
        baseColor *= mix(1.0, u.fracCracks.w, crackBand);
        emissive  += u.fracCrackGlow.rgb * u.fracCrackGlow.w * crackBand * amount;
    }

    // DISSOLVE — fbm-threshold erosion with a glowing burn edge.
    if ((mode & 2u) != 0u) {
        float scale = max(u.fracDissolveA.x, 1e-3);
        float speed = u.fracDissolveA.y;
        float n = procFbm3(wp * scale + float3(0.0, 0.0, time * speed * 0.1), 4);
        float thr = amount;
        if (u.fracDissolveB.z > 0.5) {   // radial bias from burn point
            float rad = length(wp - u.fracBurn.xyz)
                      / max(u.fracDissolveA.z, 1e-3);
            thr -= u.fracDissolveA.w * (rad - 0.5);
        }
        float edgeW = max(u.fracDissolveB.x, 1e-3);
        if (n < thr - edgeW) return true;          // fully eaten
        float edge = 1.0 - smoothstep(thr - edgeW, thr, n);
        emissive += u.fracDissolveColor.rgb * u.fracDissolveB.y * edge * amount;
    }

    // GLITCH — per-band fragment dropout (flicker-darken + digital tint).
    if ((mode & 8u) != 0u) {
        float band = floor(wp.y * u.fracGlitch.x);
        float roll = floor(time * u.fracGlitch.z);
        float h = fract(sin(dot(float2(band, roll), float2(127.1, 311.7)))
                        * 43758.5453);
        if (h < u.fracGlitch.w * amount) {
            baseColor *= 0.15;
            emissive  += float3(0.10, 0.30, 0.40) * amount;
        }
    }
    return false;
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

    // ---- ProceduralMaterialLayer: full 10-pattern triplanar dispatcher ----
    // procShape.z is the effective mix (mix * layer opacity, packed CPU
    // side). 0 → fast-path skip. Sampled in world space (triplanar) so the
    // pattern paints the physical structure regardless of UV seams.
    float procMix = u.procShape.z;
    if (procMix > 1e-3) {
        float3 procCol = sfxProceduralTriplanar(in.worldPos, N,
                                                 u.surfaceFxParams.w,
                                                 u.procColorA, u.procColorB,
                                                 u.procShape, u.procAnim);
        baseColor = mix(baseColor, procCol, procMix);
    }

    // ---- MeshFractureLayer: 4 modes (cracks/dissolve/glitch fragment-side;
    //      explode + glitch-lateral already applied in the vertex stage) ----
    float3 fractureTint = float3(0.0);
    bool fracDiscard = fracFragment(in.worldPos, u, baseColor, fractureTint);
    if (fracDiscard) discard_fragment();
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

    // ---- Area lights (representative-point, disc-equivalent) -----------
    // Karis 2013 §§ 4.1-4.3 (UE4 sphere/disc lights) + Lagarde & de Rousiers
    // 2014 closed-form Lambertian disc form factor. The AreaLightLayer's
    // safety clamp guarantees the disc never enters the structure AABB, so
    // we don't need to do a "light inside mesh" rejection here.
    float3 LoAreas = float3(0.0);
    int    areaCount = int(u.metallicCounts.y);
    int    aN = min(areaCount, MAX_AREAS);
    for (int i = 0; i < aN; i++) {
        AreaLight a = u.areas[i];
        float3 ap = a.posIntensity.xyz;
        float  ai = a.posIntensity.w;
        if (ai <= 0.0) continue;
        float3 an = normalize(a.normRange.xyz);
        float  ar = a.normRange.w;
        float3 ac = a.colorRadius.rgb;
        float  rd = max(a.colorRadius.a, 1e-4);

        float3 toLight = ap - in.worldPos;
        float  dist    = length(toLight);
        if (dist > ar) continue;
        float3 Lc      = toLight / max(dist, 1e-4);

        // One-sided panel: an is the disc normal (face direction). We want
        // the surface on the +an side, i.e. dot(an, -Lc) > 0.
        float facing = dot(an, -Lc);
        if (facing <= 0.0) continue;

        float rangeFade = saturate(1.0 - dist / ar);

        // Lagarde diffuse form factor: NdotLc * r^2 / (d^2 + r^2). Exact
        // for small solid angles, very good for larger panels. `facing`
        // is folded in so the diffuse fades smoothly at the silhouette
        // instead of popping when the panel rolls past 90°.
        float NdotLc   = max(dot(N, Lc), 0.0);
        float discR2   = rd * rd;
        float formFact = NdotLc * discR2 / max(dist * dist + discR2, 1e-4);
        float3 radDiff = ac * ai * formFact * rangeFade * facing;

        // Representative-point specular. Intersect the reflection ray with
        // the disc; if it hits, L_rep = R. Otherwise project to the closest
        // point on the disc rim and use that. Widen the GGX lobe by
        // alpha' = saturate(alpha + r / (2*dist)) to compensate for the
        // larger solid angle vs. a punctual source.
        float3 R          = reflect(-V, N);
        float  rDotLc     = dot(R, Lc);
        float3 onAxis     = Lc * rDotLc;
        float3 offAxis    = R  - onAxis;
        float  offLen     = length(offAxis);
        float3 closestOff = (offLen > 1e-5)
                          ? (offAxis / offLen) * min(offLen, rd)
                          : float3(0.0);
        float3 closestPt  = ap + closestOff;
        float3 toRep      = closestPt - in.worldPos;
        float  repDist    = length(toRep);
        float3 Lrep       = (repDist > 1e-4) ? (toRep / repDist) : Lc;

        float aOrig  = roughness;
        float aPrime = saturate(aOrig + rd / max(2.0 * dist, 1e-4));

        float NdotL = max(dot(N, Lrep), 0.0);
        float3 LoSpec = float3(0.0);
        if (NdotL > 0.0) {
            float3 H     = normalize(V + Lrep);
            float  NdotV = max(dot(N, V), 0.0);
            float  NdotH = max(dot(N, H), 0.0);
            float  VdotH = max(dot(V, H), 0.0);
            float3 F     = fresnelSchlick(VdotH, F0);
            float  D     = distributionGGX(NdotH, aPrime);
            float  G     = geometrySmith(NdotV, NdotL, aPrime);
            // Karis energy compensation: divide by alpha^2 ratio so the
            // peak doesn't brighten as the lobe widens.
            float  norm  = (aOrig * aOrig + 1e-5) / (aPrime * aPrime + 1e-5);
            float3 spec  = (D * G * F * norm)
                         / max(4.0 * NdotV * NdotL, 1e-4);
            float  specFade = saturate(1.0 - repDist / ar);
            float3 radSpec  = ac * ai * specFade * facing;
            LoSpec = spec * radSpec * NdotL;
        }

        float3 kdA = (1.0 - fresnelSchlick(NdotLc, F0)) * (1.0 - metallic);
        LoAreas += kdA * (baseColor / PI) * radDiff + LoSpec;
    }

    float3 totalLight = LoDir + LoSpots + LoAreas + emission;
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

    // ---- HologramMaterialLayer MVP splice (overlay) ----
    colorLin = sfxHologramOverlay(colorLin, N, V, in.worldPos,
                                    u.surfaceFx.x,
                                    u.surfaceFxParams.w);

    // ---- Fracture crack glow (additive on top of lit colour) ----
    colorLin += fractureTint;

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

struct GpuArea {
    glm::vec4 posIntensity;   // .xyz pos, .w intensity
    glm::vec4 normRange;      // .xyz normal, .w range
    glm::vec4 colorRadius;    // .rgb color, .a equivalent radius
};

// CPU mirror of the MSL GpuDeformOp — must match byte-for-byte. Identical
// layout to MeshDeformationLayer::GpuChain::GpuOp so we can memcpy directly.
struct GpuDeformOpC {
    glm::vec4 header;
    glm::vec4 pA;
    glm::vec4 pB;
};

struct Uniforms {
    glm::mat4 projection;
    glm::mat4 view;
    glm::mat4 model;
    glm::vec4 cameraWorldPos;
    glm::vec4 baseColorRoughness;
    glm::vec4 metallicCounts;    // .x metallic, .y areaCount, .z spotCount, .w dirCount
    glm::vec4 ambientColor;      // .rgb ambient fill
    glm::vec4 modeFlags;         // .x emitLightsOnly (0/1)
    glm::vec4 matBaseColor;      // material baseColorFactor (RGBA)
    glm::vec4 matEmissive;       // .rgb emissiveFactor
    glm::vec4 matMR;             // .x metallicFactor, .y roughnessFactor
    glm::vec4 fx;                // .x displace, .y displaceScale, .z twist
    glm::vec4 syphonMixTint;     // .x mix, .yzw tint
    glm::vec4 syphonParams;      // .x useAtlasUVs (0/1), .z flipY (0/1)
    glm::vec4 surfaceFx;         // .x holo .y proc .z deform .w fracture
    glm::vec4 surfaceFxParams;   // .x procScale .y deformScale .z fracSeed .w time
    glm::vec4 deformMeta;        // .x deformCount, .y maxDisplacement(m)
    GpuDeformOpC deformers[kRendDeformOps];
    glm::vec4 ffdCorners[kRendFfdSlots * 8];
    glm::vec4 procColorA;        // ProceduralMaterialUniforms.colorA
    glm::vec4 procColorB;        // .colorB
    glm::vec4 procShape;         // .shape (scale, contrast, mix, pattern)
    glm::vec4 procAnim;          // .anim (driftXY, timeMult, octaves)
    glm::vec4 fracMeta;          // mode, amount, seed, time
    glm::vec4 fracCracks;        // density, width, jitter, darken
    glm::vec4 fracCrackGlow;     // glowColor.rgb, glow
    glm::vec4 fracDissolveA;     // scale, speed, radius, radialBias
    glm::vec4 fracDissolveB;     // edgeWidth, edgeGlow, useRadial, jitterSpeed
    glm::vec4 fracBurn;          // burnPoint.xyz
    glm::vec4 fracDissolveColor; // edgeColor.rgb
    glm::vec4 fracExplode;       // shardDensity, jitter, spin, strength
    glm::vec4 fracMeshCenter;    // centroid.xyz, explodeCap(m)
    glm::vec4 fracGlitch;        // freq, magnitude, speed, dropout
    GpuDir    dirs[kMaxDirs];
    GpuSpot   spots[kMaxSpots];
    GpuArea   areas[kMaxAreas];
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
    const std::vector<VirtualSpot>& virtualSpots,
    const glm::vec4& surfaceFx,
    const glm::vec4& surfaceFxParams,
    const std::vector<const AreaLightLayer*>& areas,
    const MeshDeformationLayer* deformLayer,
    const ProceduralMaterialUniforms* procMat,
    const MeshFractureLayer* fracLayer)
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

    // Pack area lights. positionWorld / normalWorld / effectiveR were
    // already refreshed by StructureLayer's bus walk via a->update(ctx).
    // We just apply LFO + opacity to the static intensity here.
    int areaCount = std::min(static_cast<int>(areas.size()), kMaxAreas);
    GpuArea areasPacked[kMaxAreas]{};
    int areaWritten = 0;
    for (int i = 0; i < areaCount; ++i) {
        const AreaLightLayer* a = areas[i];
        if (!a) continue;
        float inten = a->intensity + a->intensityLFO.eval(t);
        inten      *= a->opacity;
        if (inten <= 0.0f) continue;
        areasPacked[areaWritten].posIntensity =
            glm::vec4(a->positionWorld, inten);
        areasPacked[areaWritten].normRange    =
            glm::vec4(a->normalWorld,    a->range);
        areasPacked[areaWritten].colorRadius  =
            glm::vec4(a->color,          a->effectiveR);
        ++areaWritten;
    }
    areaCount = areaWritten;

    // Snapshot the deformer chain once (identical for every mesh). The
    // snapshot evaluates each op's effective intensity through the
    // modulator bank. count == 0 → vertex shader falls back to the MVP
    // wave (or identity if surfaceFx.z is also 0).
    MeshDeformationLayer::GpuChain deformChain;
    int deformOpCount = 0;
    float deformMaxDisp = 1.5f;
    if (deformLayer) {
        const ModulatorBank* dmods = ctx.scene ? &ctx.scene->modulators
                                               : nullptr;
        deformChain    = deformLayer->snapshot(t, dmods);
        deformOpCount  = deformChain.count;
        deformMaxDisp  = deformLayer->maxDisplacement;
    }

    // Pack the fracture params once (identical for every mesh). Amount uses
    // the layer's real master amount + modulator bank (NOT the inherited
    // opacity). mode==0 / amount==0 → shader fast-path skip.
    glm::vec4 fracMeta(0.0f), fracCracks(0.0f), fracCrackGlow(0.0f);
    glm::vec4 fracDissolveA(0.0f), fracDissolveB(0.0f), fracBurn(0.0f);
    glm::vec4 fracDissolveColor(0.0f), fracExplode(0.0f);
    glm::vec4 fracMeshCenter(0.0f), fracGlitch(0.0f);
    if (fracLayer && fracLayer->mode != 0u) {
        const ModulatorBank* fmods = ctx.scene ? &ctx.scene->modulators
                                               : nullptr;
        float amt = fracLayer->effectiveAmount(t, fmods)
                  * std::clamp(fracLayer->opacity, 0.0f, 1.0f);
        if (amt > 1e-3f) {
            glm::vec3 centroid = ctx.scene ? ctx.scene->centroid
                                           : glm::vec3(0.0f);
            fracMeta      = glm::vec4(static_cast<float>(fracLayer->mode),
                                       amt,
                                       static_cast<float>(fracLayer->id) * 0.137f,
                                       static_cast<float>(t));
            fracCracks    = glm::vec4(fracLayer->crackDensity,
                                       fracLayer->crackWidth,
                                       fracLayer->crackJitter,
                                       fracLayer->crackDarken);
            fracCrackGlow = glm::vec4(fracLayer->crackGlowColor,
                                       fracLayer->crackGlow);
            fracDissolveA = glm::vec4(fracLayer->dissolveScale,
                                       fracLayer->dissolveSpeed,
                                       fracLayer->dissolveRadius,
                                       fracLayer->dissolveRadialBias);
            fracDissolveB = glm::vec4(fracLayer->dissolveEdgeWidth,
                                       fracLayer->dissolveEdgeGlow,
                                       fracLayer->useRadialDissolve ? 1.0f : 0.0f,
                                       fracLayer->crackJitterSpeed);
            fracBurn      = glm::vec4(fracLayer->burnPoint, 0.0f);
            fracDissolveColor = glm::vec4(fracLayer->dissolveEdgeColor, 0.0f);
            fracExplode   = glm::vec4(fracLayer->shardDensity,
                                       fracLayer->shardJitter,
                                       fracLayer->shardSpin,
                                       fracLayer->explodeStrength);
            // Hard explode cap (m): shards stay near the structure so the
            // projection illusion never breaks (research finding).
            fracMeshCenter = glm::vec4(centroid, 0.5f);
            fracGlitch    = glm::vec4(fracLayer->glitchFrequency,
                                       fracLayer->glitchMagnitude,
                                       fracLayer->glitchSpeed,
                                       fracLayer->glitchDropout);
        }
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
                                          static_cast<float>(areaCount),
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
        u.surfaceFx       = surfaceFx;
        u.surfaceFxParams = surfaceFxParams;
        // Deformer chain (snapshot is identical for every mesh).
        u.deformMeta = glm::vec4(static_cast<float>(deformOpCount),
                                  deformMaxDisp, 0.0f, 0.0f);
        if (deformOpCount > 0) {
            // GpuChain::GpuOp == GpuDeformOpC byte-for-byte (3× vec4).
            std::memcpy(u.deformers, deformChain.ops.data(),
                         sizeof(GpuDeformOpC) * kRendDeformOps);
            // ffdSlots is std::array<std::array<vec4,8>,4> — contiguous,
            // matches u.ffdCorners[32].
            std::memcpy(u.ffdCorners, deformChain.ffdSlots.data(),
                         sizeof(glm::vec4) * kRendFfdSlots * 8);
        }
        // Procedural material block (10-pattern triplanar). disabled* when
        // no layer present → shape.z (mix) == 0 → shader fast-path skip.
        if (procMat) {
            u.procColorA = procMat->colorA;
            u.procColorB = procMat->colorB;
            u.procShape  = procMat->shape;
            u.procAnim   = procMat->anim;
        } else {
            u.procShape = glm::vec4(1.0f, 1.0f, 0.0f, 0.0f);  // mix=0
        }
        // Fracture params (fracMeta.x == 0 → shader fast-path skip).
        u.fracMeta          = fracMeta;
        u.fracCracks        = fracCracks;
        u.fracCrackGlow     = fracCrackGlow;
        u.fracDissolveA     = fracDissolveA;
        u.fracDissolveB     = fracDissolveB;
        u.fracBurn          = fracBurn;
        u.fracDissolveColor = fracDissolveColor;
        u.fracExplode       = fracExplode;
        u.fracMeshCenter    = fracMeshCenter;
        u.fracGlitch        = fracGlitch;
        std::memcpy(u.dirs,  dirsPacked,  sizeof(GpuDir)  * kMaxDirs);
        std::memcpy(u.spots, spotsPacked, sizeof(GpuSpot) * kMaxSpots);
        std::memcpy(u.areas, areasPacked, sizeof(GpuArea) * kMaxAreas);

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
