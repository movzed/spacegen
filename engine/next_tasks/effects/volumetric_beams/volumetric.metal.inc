// volumetric.metal.inc — MSL source for the VolumetricBeamLayer post-process.
//
// Usage in MetalRenderer.cpp (matching the kStructurePbrMSL pattern):
//
//   constexpr const char* kVolumetricBeamMSL = R"MSL(
//   #include "volumetric.metal.inc"
//   )MSL";
//
// or — simpler — paste the entire contents between the R"MSL( ... )MSL"
// delimiters of a new `kVolumetricBeamMSL` constant. See INTEGRATION.md
// for the exact diff.
//
// This is a fullscreen-quad post-process. Vertex shader emits 4 covers of
// NDC space; fragment shader does the raymarch.

#include <metal_stdlib>
using namespace metal;

constant float PI_VOL    = 3.14159265359;
constant int   MAX_SPOTS_VOL = 32;

// ---- Vertex stage ----------------------------------------------------------
// Standard fullscreen triangle (or quad). The vertex shader takes no vertex
// buffer and emits NDC + matching UV from the vertex_id.

struct VOut {
    float4 pos [[position]];
    float2 ndc;
};

vertex VOut vb_vs(uint vid [[vertex_id]]) {
    // Two-triangle quad covering [-1,1]^2. Hand-unrolled.
    const float2 verts[6] = {
        float2(-1.0, -1.0), float2( 1.0, -1.0), float2(-1.0,  1.0),
        float2( 1.0, -1.0), float2( 1.0,  1.0), float2(-1.0,  1.0),
    };
    VOut o;
    o.pos = float4(verts[vid], 0.0, 1.0);
    o.ndc = verts[vid];
    return o;
}

// ---- Fragment stage --------------------------------------------------------
// Per-fragment we:
//   1. Reconstruct the view ray in world space from the inverse view-proj.
//   2. Read the linear scene depth from the depth attachment texture (sampled
//      as a float, since it's Depth32Float — accessed via depth2d<float>).
//   3. Convert that to a world-space "front surface" distance along the ray.
//   4. Raymarch from camera near to that distance, accumulating single-scattering
//      from each spotlight using the Henyey-Greenstein phase function.
//   5. Blend additively on top of the existing color (provided as a sampled
//      texture so we can read+write the same target safely).

struct VolSpot {
    float4 posIntensity;   // .xyz world pos,    .w intensity (* opacity * perFxMul)
    float4 dirRange;       // .xyz dir (FROM light, normalized), .w range
    float4 colorInner;     // .rgb color,        .a cos(innerHalfAngle)
    float4 outerMul;       // .x cos(outerHalfAngle)
};

struct VolUniforms {
    float4x4 invViewProj;        // NDC + depth -> world
    float4   cameraWorldPos;     // .xyz position, .w camera near
    float4   params0;            // .x density, .y anisotropy g, .z sampleCount, .w jitterStrength
    float4   params1;            // .xyz tint, .w beerLambert (0/1)
    float4   counts;             // .x spotCount, .y layerOpacity, .z resW, .w resH
    VolSpot  spots[MAX_SPOTS_VOL];
};

// ---- Henyey-Greenstein phase function -------------------------------------
// Φ_HG(cosθ, g) = (1 - g²) / (4π · (1 + g² - 2g·cosθ)^(3/2))
// We DROP the 1/(4π) factor and roll it into the master density slider —
// the operator's "density" slider absorbs the constant so the slider's
// numeric value behaves intuitively at the default range. See README.

static float hgPhase(float cosTheta, float g) {
    float g2 = g * g;
    float denom = 1.0 + g2 - 2.0 * g * cosTheta;
    // Guard the singularity (denom -> 0 when g near +/-1 and cosTheta = ±1).
    denom = max(denom, 1e-4);
    return (1.0 - g2) / pow(denom, 1.5);
}

// ---- Cheap deterministic 2D hash for per-pixel jitter ---------------------
// IQ canon — hash float2 -> float in [0, 1).
static float jitterHash(float2 p, float frameOffset) {
    p += frameOffset;
    float3 p3 = fract(float3(p.xyx) * float3(443.8975, 397.2973, 491.1871));
    p3 += dot(p3, p3.yzx + 19.19);
    return fract((p3.x + p3.y) * p3.z);
}

// ---- World-space ray reconstruction ----------------------------------------
// Given NDC.xy and a depth in [0,1] (Metal convention), reconstruct the
// world-space position. Used to find the FAR end of the integration ray
// (the front-most surface depth).

static float3 worldFromNdcDepth(float2 ndc, float depth, float4x4 invVP) {
    // Metal NDC z is in [0, 1] (Vulkan/D3D convention). Our depth texture
    // is Depth32Float written by the structure pass with depth_value = z/w
    // in [0,1], so we can use the depth sample directly.
    float4 clip = float4(ndc.x, ndc.y, depth, 1.0);
    float4 world = invVP * clip;
    return world.xyz / max(world.w, 1e-6);
}

fragment float4 vb_fs(VOut in [[stage_in]],
                       constant VolUniforms& u [[buffer(0)]],
                       texture2d<float>      sceneColor [[texture(0)]],
                       depth2d<float>        sceneDepth [[texture(1)]],
                       sampler               smp        [[sampler(0)]])
{
    // ---- Sample the existing color + depth at this pixel -------------------
    // ndc.y in Metal sample-space is flipped vs render-space; we use the
    // raw NDC.xy from the VS for both.
    float2 uv = float2(in.ndc.x * 0.5 + 0.5,
                       in.ndc.y * 0.5 + 0.5);
    // Sample's UV.y is flipped relative to NDC for the framebuffer attachment.
    float2 uvFlipped = float2(uv.x, 1.0 - uv.y);

    float4 baseColor = sceneColor.sample(smp, uvFlipped);
    float  d         = sceneDepth.sample(smp, uvFlipped);

    // No surface in front (depth = 1 == clear value) means we'd be integrating
    // to infinity. Cap at d=0.9999 so we still get cones in the sky.
    d = min(d, 0.9999);

    // ---- Reconstruct the camera origin and the far point of the ray --------
    float3 camPos = u.cameraWorldPos.xyz;
    float  camNear = u.cameraWorldPos.w;
    float3 farPos = worldFromNdcDepth(in.ndc, d, u.invViewProj);

    float3 rayVec = farPos - camPos;
    float  rayLen = length(rayVec);
    if (rayLen < 1e-3) return baseColor;
    float3 V = rayVec / rayLen;                   // unit view direction

    // Clamp the ray start so we don't sample inside the camera near plane.
    float startT = max(camNear, 0.05);
    float endT   = rayLen;
    if (endT <= startT) return baseColor;

    // ---- Master parameters -------------------------------------------------
    float density       = u.params0.x;
    float g             = u.params0.y;
    int   N             = max(4, int(u.params0.z));
    float jitterStrength = u.params0.w;
    float3 tint         = u.params1.xyz;
    bool   beerLambert  = u.params1.w > 0.5;
    int    spotCount    = min(int(u.counts.x), MAX_SPOTS_VOL);
    float  layerOpacity = u.counts.y;

    // Step length (uniform stratification along the ray).
    float invN  = 1.0 / float(N);
    float stepT = (endT - startT) * invN;

    // Per-pixel deterministic jitter offset in [0, 1). Combined with the
    // frame index (passed via params0 if we want temporal AA; here we keep
    // it spatial-only to avoid temporal shimmer on a static camera).
    float jitter01 = jitterHash(in.ndc * 0.5 + 0.5, 0.0);
    float jitter   = (jitter01 - 0.5) * jitterStrength;   // [-jitterStrength/2, +jitterStrength/2)

    // ---- Raymarch ----------------------------------------------------------
    // Inscatter accumulator (linear RGB) + transmittance (Beer-Lambert
    // version) or path-length accumulator (off-Beer version).
    float3 inscatter = float3(0.0);
    float  transmittance = 1.0;

    for (int i = 0; i < N; ++i) {
        // Sample fraction along the ray, with jitter.
        float ti = (float(i) + 0.5 + jitter) * invN;
        ti = clamp(ti, 0.0, 1.0);
        float dist = mix(startT, endT, ti);
        float3 X = camPos + V * dist;

        // For each fixture: cone test + range fade + HG phase.
        // The inner loop dominates fragment cost: spotCount × N.
        // With kMaxSpots=32 and N=48 the inner per-fragment is ~1500 ops.
        float3 lightAtX = float3(0.0);

        for (int s = 0; s < spotCount; ++s) {
            VolSpot sp = u.spots[s];
            float si = sp.posIntensity.w;
            if (si <= 0.0) continue;

            float3 toLight = sp.posIntensity.xyz - X;
            float  dlight  = length(toLight);
            float  rg      = sp.dirRange.w;
            if (dlight > rg || dlight < 1e-4) continue;

            float3 L  = toLight / dlight;          // unit FROM surface TO light
            float3 D  = normalize(sp.dirRange.xyz); // unit FROM light TO scene

            // Cone test: dot(-L, D) is high when X is inside the cone.
            float spotCos = dot(-L, D);
            float oc = sp.outerMul.x;
            float ic = sp.colorInner.a;
            if (spotCos < oc) continue;
            float cone = smoothstep(oc, ic, spotCos);

            // Range fade — linear, matching the structure shader.
            float rangeFade = saturate(1.0 - dlight / rg);

            // HG phase: cosθ is angle between view direction V and the
            // light's direction towards the camera, which is -L (light → eye
            // = - (eye → light)).
            // Convention: forward-scatter (g>0) peaks when V and -L are
            // parallel — i.e. looking down the beam from behind it.
            float cosTheta = dot(V, -L);
            float phase = hgPhase(cosTheta, g);

            float3 radiance = sp.colorInner.rgb * si * cone * rangeFade;
            lightAtX += radiance * phase;
        }

        // Single-scattering contribution at this step: σ_s · phase·radiance · dx
        // (phase is already folded into lightAtX; density is σ_s; stepT is dx).
        float3 inscatStep = lightAtX * density * stepT * transmittance;
        inscatter += inscatStep;

        if (beerLambert) {
            // Extinction along the ray. Cheap assumption: σ_t = σ_s
            // (no separate absorption — operator can dial via density).
            transmittance *= exp(-density * stepT);
            // Aggressive early-out when the ray is fully extinguished.
            if (transmittance < 0.005) break;
        }
    }

    // Tint + layer opacity.
    float3 result = inscatter * tint * layerOpacity;

    // ---- Composite over the existing color --------------------------------
    // Per the alpha contract: this is a generator, alpha increases by the
    // luminance of what we just added. The blend mode (Add) is honored by
    // the pipeline's color-attachment blending: we return premultiplied
    // additive (rgb = result, alpha = luma(result)).
    float lum = dot(result, float3(0.2126, 0.7152, 0.0722));
    float addAlpha = saturate(lum);

    // Additive blend. The pipeline's blend state is configured to:
    //   dstColor = srcColor.rgb + dstColor.rgb
    //   dstAlpha = saturate(srcAlpha + dstAlpha)
    // so we just return (result, addAlpha) and the hardware does the rest.
    return float4(result, addAlpha);
}
