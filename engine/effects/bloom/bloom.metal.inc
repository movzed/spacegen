// bloom.metal.inc — MSL kernel string for BloomEffect.
//
// This file is `#include`d as a C++ raw-string literal by BloomEffect.cpp
// (the contents below sit inside `R"MSL( ... )MSL"` brackets at the call
// site). Keeping the shader in its own file makes the algorithm reviewable
// independently of the C++ scaffolding, and gives editors syntax-highlighting
// when opened on its own.
//
// References:
//   Karis 2013    — Real Shading in Unreal Engine 4 (SIGGRAPH PBS course)
//   Jimenez 2014  — Next Gen Post Processing in CoD: Advanced Warfare
//
// Pipeline functions exported by this file:
//   fs_threshold   — Karis soft-knee bright pass
//   fs_downsample  — Jimenez 13-tap downsample (Karis-average on first pass)
//   fs_upsample    — Jimenez 9-tap tent upsample (additively blended)
//   fs_composite   — final mix(scene, bloom * tint, intensity)
// Plus a shared fullscreen-quad vertex shader `vs_fullscreen`.

#include <metal_stdlib>
using namespace metal;

// Fullscreen quad: emit 3 vertices (a single oversized triangle covers the
// whole NDC viewport without a vertex buffer — saves a per-pass binding).
// Indices 0,1,2 → positions { (-1,-1), (3,-1), (-1, 3) } in clip space.
struct VOut {
    float4 position [[position]];
    float2 uv;
};

vertex VOut vs_fullscreen(uint vid [[vertex_id]]) {
    VOut o;
    float2 p = float2( (vid << 1) & 2, vid & 2);   // (0,0), (2,0), (0,2)
    o.position = float4(p * 2.0 - 1.0, 0.0, 1.0);  // → (-1,-1),(3,-1),(-1,3)
    // UV in [0,1], V flipped to match Metal's top-left texture origin so
    // downstream samples line up with the source colorTarget's pixel rows.
    o.uv = float2(p.x, 1.0 - p.y);
    return o;
}

// ---- Threshold-pass uniforms ----
// Packed into a single float4 for tight bindings.
//   x = threshold
//   y = softKnee
//   z, w = unused
struct ThresholdU {
    float4 params;
};

// Karis soft-knee bright pass. Returns the input color scaled so that:
//   luma <= threshold - knee  → 0
//   luma >= threshold + knee  → unmodified color
//   middle band               → quadratic ramp (smooth fade-in)
// `knee = 0` recovers the hard cutoff `max(0, luma - threshold) * color`.
fragment float4 fs_threshold(VOut in [[stage_in]],
                              texture2d<float> src [[texture(0)]],
                              sampler          smp [[sampler(0)]],
                              constant ThresholdU& u [[buffer(0)]])
{
    float3 color = src.sample(smp, in.uv).rgb;
    float  luma  = dot(color, float3(0.2126, 0.7152, 0.0722));

    float  threshold = u.params.x;
    float  knee      = max(u.params.y, 1e-4);

    // Soft-knee curve (Karis 2013 / UE4 PostProcessBloom.usf)
    float soft = clamp(luma - threshold + knee, 0.0, 2.0 * knee);
    soft       = (soft * soft) / (4.0 * knee);
    float hard = max(luma - threshold, 0.0);
    float boost = max(soft, hard) / max(luma, 1e-4);

    return float4(color * boost, 1.0);
}

// ---- Downsample uniforms ----
//   xy = source texel size (1/srcW, 1/srcH)
//   z  = firstDownsample flag (1.0 = apply Karis-average firefly filter)
//   w  = unused
struct DownU {
    float4 params;
};

// Karis average. Weights four input colors by 1/(1+luma) — bright outliers
// (fireflies) contribute less, so the surviving energy is the average of
// the "calm" pixels rather than dominated by the single hot one. Used only
// on the first downsample (mip[0] → mip[1]) where the bright pass still
// contains sharp single-pixel outliers; subsequent levels have already been
// averaged enough that the filter buys nothing.
static float3 karisAverage(float3 a, float3 b, float3 c, float3 d) {
    float wa = 1.0 / (1.0 + dot(a, float3(0.2126, 0.7152, 0.0722)));
    float wb = 1.0 / (1.0 + dot(b, float3(0.2126, 0.7152, 0.0722)));
    float wc = 1.0 / (1.0 + dot(c, float3(0.2126, 0.7152, 0.0722)));
    float wd = 1.0 / (1.0 + dot(d, float3(0.2126, 0.7152, 0.0722)));
    float wsum = wa + wb + wc + wd;
    return (a * wa + b * wb + c * wc + d * wd) * (1.0 / wsum);
}

// Jimenez 2014 13-tap "partial-Karis-average" downsample. The 13 source
// taps form 5 overlapping 2×2 sub-quads (one center + 4 corners). Each
// sub-quad's average is contributed with a fixed weight (0.5 for center,
// 0.125 for each corner) which sums to 1.0. On the first pass, each
// sub-quad is averaged with karisAverage() instead of arithmetic mean.
// Slide 153, "Downsample with partial Karis average".
fragment float4 fs_downsample(VOut in [[stage_in]],
                               texture2d<float> src [[texture(0)]],
                               sampler          smp [[sampler(0)]],
                               constant DownU&  u   [[buffer(0)]])
{
    float2 t   = u.params.xy;          // source texel size
    float2 uv  = in.uv;
    bool   useKaris = u.params.z > 0.5;

    // 13 source samples: layout per Jimenez 2014 (offsets in texels).
    //   a b c
    //    j k
    //   d e f
    //    l m
    //   g h i
    float3 a = src.sample(smp, uv + t * float2(-2.0,  2.0)).rgb;
    float3 b = src.sample(smp, uv + t * float2( 0.0,  2.0)).rgb;
    float3 c = src.sample(smp, uv + t * float2( 2.0,  2.0)).rgb;

    float3 d = src.sample(smp, uv + t * float2(-2.0,  0.0)).rgb;
    float3 e = src.sample(smp, uv                          ).rgb;
    float3 f = src.sample(smp, uv + t * float2( 2.0,  0.0)).rgb;

    float3 g = src.sample(smp, uv + t * float2(-2.0, -2.0)).rgb;
    float3 h = src.sample(smp, uv + t * float2( 0.0, -2.0)).rgb;
    float3 i = src.sample(smp, uv + t * float2( 2.0, -2.0)).rgb;

    float3 j = src.sample(smp, uv + t * float2(-1.0,  1.0)).rgb;
    float3 k = src.sample(smp, uv + t * float2( 1.0,  1.0)).rgb;
    float3 l = src.sample(smp, uv + t * float2(-1.0, -1.0)).rgb;
    float3 m = src.sample(smp, uv + t * float2( 1.0, -1.0)).rgb;

    // Five 2×2 averages: q0 = inner quad, q1..q4 = four outer quads.
    // Either Karis average (firefly suppression) or arithmetic mean.
    float3 q0, q1, q2, q3, q4;
    if (useKaris) {
        q0 = karisAverage(j, k, l, m);
        q1 = karisAverage(a, b, d, e);
        q2 = karisAverage(b, c, e, f);
        q3 = karisAverage(d, e, g, h);
        q4 = karisAverage(e, f, h, i);
    } else {
        q0 = (j + k + l + m) * 0.25;
        q1 = (a + b + d + e) * 0.25;
        q2 = (b + c + e + f) * 0.25;
        q3 = (d + e + g + h) * 0.25;
        q4 = (e + f + h + i) * 0.25;
    }

    // Slide 153: center has weight 0.5, four corners have weight 0.125 each.
    // Total = 0.5 + 4 × 0.125 = 1.0  → energy-preserving.
    float3 result = q0 * 0.5 + (q1 + q2 + q3 + q4) * 0.125;
    return float4(max(result, float3(0.0)), 1.0);
}

// ---- Upsample uniforms ----
//   xy = source (lower-mip) texel size
//   z  = scatter (Jimenez's "filter radius", normally 1.0; > 1 widens halo)
//   w  = unused
struct UpU {
    float4 params;
};

// Jimenez 2014 9-tap tent filter. A 3×3 tent kernel sums to 16 with weights
//   1 2 1
//   2 4 2     → divide by 16 at the end
//   1 2 1
// The result is additively blended onto the destination (the previous mip
// level's downsample), so the upsample shader returns just the tent sum —
// the pipeline state sets blend = One + One. Slide 162, "Upsample with
// tent filter".
fragment float4 fs_upsample(VOut in [[stage_in]],
                             texture2d<float> src [[texture(0)]],
                             sampler          smp [[sampler(0)]],
                             constant UpU&    u   [[buffer(0)]])
{
    float2 t  = u.params.xy * u.params.z;   // source texel size × scatter
    float2 uv = in.uv;

    float3 a = src.sample(smp, uv + t * float2(-1.0,  1.0)).rgb;
    float3 b = src.sample(smp, uv + t * float2( 0.0,  1.0)).rgb;
    float3 c = src.sample(smp, uv + t * float2( 1.0,  1.0)).rgb;

    float3 d = src.sample(smp, uv + t * float2(-1.0,  0.0)).rgb;
    float3 e = src.sample(smp, uv                         ).rgb;
    float3 f = src.sample(smp, uv + t * float2( 1.0,  0.0)).rgb;

    float3 g = src.sample(smp, uv + t * float2(-1.0, -1.0)).rgb;
    float3 h = src.sample(smp, uv + t * float2( 0.0, -1.0)).rgb;
    float3 i = src.sample(smp, uv + t * float2( 1.0, -1.0)).rgb;

    // Weights: 1·corners + 2·edges + 4·center, /16
    float3 result = (a + c + g + i) * (1.0 / 16.0)
                  + (b + d + f + h) * (2.0 / 16.0)
                  +  e              * (4.0 / 16.0);
    return float4(result, 1.0);
}

// ---- Composite uniforms ----
//   xyz = tint (multiplied with the bloom before mix)
//   w   = intensity in [0, 2]
struct CompU {
    float4 tintIntensity;
};

// Final composite: read the original scene and the upsampled bloom mip[0],
// return mix(scene, bloom * tint, intensity). Both inputs are sampled
// bilinearly so the bloom hits the same logical pixels as the scene even
// if mip[0] has been ½-resolution-resampled multiple times.
fragment float4 fs_composite(VOut in [[stage_in]],
                              texture2d<float> scene [[texture(0)]],
                              texture2d<float> bloom [[texture(1)]],
                              sampler          smp   [[sampler(0)]],
                              constant CompU&  u     [[buffer(0)]])
{
    float3 s = scene.sample(smp, in.uv).rgb;
    float3 b = bloom.sample(smp, in.uv).rgb * u.tintIntensity.xyz;
    float  k = clamp(u.tintIntensity.w, 0.0, 2.0);
    // For intensity > 1 we let bloom over-boost (saturated-sensor look).
    float3 outRgb = mix(s, b, min(k, 1.0)) + b * max(k - 1.0, 0.0);
    return float4(outRgb, 1.0);
}
