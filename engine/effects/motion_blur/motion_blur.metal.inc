// motion_blur.metal.inc — MSL fragment shader source for MotionBlurEffect.
//
// Included as a raw-string in MotionBlurEffect.cpp. Compiled into a Metal
// library once on first render; the resulting pipeline is cached on the
// effect instance.
//
// Two paths are implemented in the same shader, branched by a single
// uniform flag `useCameraOnly`. Each path produces a "velocity" vector
// in screen-space UV units (i.e. delta in [0,1] over the framebuffer), then
// gathers `sampleCount` color taps along it with a smoothstep tap-weight
// kernel (peak at center, fade at ends — McGuire 2012 §2).

R"MSL(
#include <metal_stdlib>
using namespace metal;

// Vertex stage: a fullscreen triangle generated from vertex_id, no buffers.
// Standard "big triangle" trick avoids the diagonal seam of two-tri quads.
struct VsOut {
    float4 position [[position]];
    float2 uv;
};

vertex VsOut mb_vs(uint vid [[vertex_id]]) {
    // (0,0)-(2,0)-(0,2) in clip space → (0,0)-(2,0)-(0,2) in UV after the
    // /2 + 0.5 below. The triangle covers the framebuffer; the pixels
    // outside [0,1]² are clipped.
    float2 ndc = float2((vid << 1) & 2, vid & 2);
    VsOut o;
    o.position = float4(ndc * 2.0 - 1.0, 0.0, 1.0);
    // Metal UV: origin at top-left. We flip Y here so v=0 is at the top
    // (matches the structure pass's color attachment convention).
    o.uv = float2(ndc.x, 1.0 - ndc.y);
    return o;
}

struct MbUniforms {
    // Camera-only path: this is `VP_{t-1} · inverse(VP_t)`. Lets us go
    // from current NDC → previous NDC in one matrix multiply per pixel.
    float4x4 prevVPxInvVP;

    // Knobs.
    float    sampleCount;       // float so we can do float division in shader
    float    shutter;
    float    intensity;
    float    useCameraOnly;     // 1.0 = path A, 0.0 = path B
    float    frameJitter;       // [0,1) per-frame phase for the hash dither
    float    pad0;
    float    pad1;
    float    pad2;
};

// Small per-pixel hash. Returns a scalar in [0,1). Used to jitter the
// sample positions so the discrete tap count doesn't quantize into visible
// banding on slow motion.
static float mb_hash(float2 p, float seed) {
    float3 p3 = fract(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33 + seed);
    return fract((p3.x + p3.y) * p3.z);
}

// Smoothstep falloff that's 0 at the tap-line ends, 1 at the center.
// `t` is the normalized position along the trail in [0,1].
static float mb_weight(float t) {
    // 4 · t · (1 - t) is the classic "parabolic" filter with area 2/3.
    // Smoothstep is slightly more forgiving at the ends. McGuire 2012 §2.
    return smoothstep(0.0, 1.0, 1.0 - abs(t * 2.0 - 1.0));
}

fragment float4 mb_fs(VsOut in [[stage_in]],
                      constant MbUniforms& U [[buffer(0)]],
                      texture2d<float, access::sample> colorTex [[texture(0)]],
                      // Path B only — bound to a 1x1 zero texture in path A
                      // so the same shader can be used by both paths
                      // without conditional binding code on the C++ side.
                      texture2d<float, access::sample> velocityTex [[texture(1)]],
                      sampler smp [[sampler(0)]])
{
    float2 uv  = in.uv;
    float4 src = colorTex.sample(smp, uv);

    // ---- Reconstruct screen-space velocity (in UV units, i.e. [0,1]) ----
    float2 velocity;
    if (U.useCameraOnly > 0.5) {
        // -------- Path A: camera-only --------
        // Convert UV to current-frame NDC. NDC.y is flipped vs UV.
        float2 ndcNow = float2(uv.x * 2.0 - 1.0,
                                (1.0 - uv.y) * 2.0 - 1.0);
        // Project to previous frame. We assume z=0 (mid-frustum plane) —
        // exact along the camera forward, smoothly degrading off-axis.
        // For projection mapping this approximation is excellent: the
        // mapped surface lives at a roughly known distance and the camera
        // typically doesn't move at all (so the delta is tiny anyway).
        float4 cn   = float4(ndcNow, 0.0, 1.0);
        float4 prev = U.prevVPxInvVP * cn;
        // Guard against bad reprojections (e.g. point behind previous
        // camera). When prev.w flips sign, fall back to zero velocity.
        if (prev.w <= 1e-4) {
            velocity = float2(0.0);
        } else {
            float2 prevNDC = prev.xy / prev.w;
            // currentNDC - prevNDC is in NDC units (range ~[-2, +2]).
            // Convert to UV (range [-1, +1] then *0.5).
            float2 dNDC = ndcNow - prevNDC;
            // NDC.y inverted vs UV.y → flip the y component.
            velocity = float2(dNDC.x, -dNDC.y) * 0.5;
        }
    } else {
        // -------- Path B: per-object velocity buffer --------
        // The structure pass wrote
        //   velocity.rg = (currClipPos.xy/curr.w - prevClipPos.xy/prev.w) * 0.5
        // i.e. already in [-1, +1] / 2 = [-0.5, +0.5] UV units.
        // Sample once at the current pixel. (Smarter: NeighborMax pre-pass
        // from McGuire 2012 §2.2 — see TODO at bottom.)
        velocity = velocityTex.sample(smp, uv).rg;
    }

    // ---- Apply shutter and early-out on negligible motion ----
    velocity *= U.shutter;
    float vlen = length(velocity);
    if (vlen < 1e-4) {
        return src;
    }

    // Clamp velocity to a sane radius (4% of the framebuffer) — runaway
    // velocities from numerical noise in path A's reprojection would
    // otherwise produce comet trails the size of the screen on the first
    // frame after enabling.
    float maxLen = 0.04;
    if (vlen > maxLen) {
        velocity *= maxLen / vlen;
        vlen = maxLen;
    }

    // ---- Tap loop ----
    int   N = int(clamp(U.sampleCount, 4.0, 32.0));
    float jitter = mb_hash(in.position.xy, U.frameJitter) - 0.5;

    float4 accum = float4(0.0);
    float  wsum  = 0.0;
    for (int i = 0; i < N; ++i) {
        // Position along trail in [-0.5, +0.5], jittered by ~½ tap to break
        // banding (the trail is symmetric around the pixel).
        float t = (float(i) + 0.5) / float(N) - 0.5;
        float t_jittered = t + jitter / float(N);

        // Tap weight uses the *unjittered* normalized position so the
        // peak stays centered.
        float w = mb_weight(t + 0.5);

        float2 sampleUv = uv + velocity * t_jittered;
        float4 c = colorTex.sample(smp, sampleUv);
        accum += c * w;
        wsum  += w;
    }
    float4 smeared = (wsum > 1e-5) ? (accum / wsum) : src;

    // ---- Blend with original ----
    return mix(src, smeared, clamp(U.intensity, 0.0, 1.0));
}

// TODO (v2): McGuire 2012 §2.2 NeighborMax 8×8 tile pre-pass. Two extra
// half-res passes:
//   1. Tile pass:  for each 8×8 tile, output max-by-length velocity in
//                  that tile.
//   2. Neighbor:   for each tile, output max-by-length over the 3×3 tile
//                  neighborhood.
// The main pass then reads neighborMax to pick a per-pixel sampling vector
// that captures incoming smear from offscreen-moving objects (essential
// for fast-moving thin features). Path B benefits most; in our projection
// mapping use case the simpler direct gather is already plausible.
)MSL"
