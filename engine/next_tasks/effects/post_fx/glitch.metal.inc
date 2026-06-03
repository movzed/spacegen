// glitch.metal.inc — MSL fragment shader for GlitchEffect.
// Included as a C++ raw-string literal from GlitchEffect.cpp.
//
// Single fullscreen-quad pass with 5 toggleable sub-effects gated by
// per-sub-effect threshold values. All randomness is hashed from
// (uv-cell, beatIndex, seed) so the pattern is stable within a beat
// and re-rolls on the next downbeat.

R"MSL(
#include <metal_stdlib>
using namespace metal;

struct GlitchUniforms {
    // .x effectiveAmount (0..1) — combined master + env + mod
    // .y beatPhase (0..1)        — phase within current beat (0 = on beat)
    // .z beatIndex (float-cast int) — integer beat counter for re-roll
    // .w timeSeconds              — raw time (used as fallback when bpm=0)
    float4 amountBeat;

    // .x seed (float-cast int) for hashing
    // .y bandThicknessPx
    // .z 1.0/widthPx
    // .w 1.0/heightPx
    float4 paramsA;

    // Per-sub-effect enable flags (1 = on, 0 = off).
    // .x bands  .y rgbSwap  .z dropout  .w tear
    float4 enables;

    // Per-sub-effect thresholds (effect fires when effectiveAmount > thr).
    // .x bands  .y rgbSwap  .z dropout  .w tear
    float4 thresholds;

    // Flash sub-effect packed separately (its threshold + enable + color).
    // .x enable  .y threshold  .zw unused
    float4 flashCtrl;
    // .xyz flash color           .w unused
    float4 flashColor;
};

struct VsOut {
    float4 position [[position]];
    float2 uv;
};

vertex VsOut glitch_vs(uint vid [[vertex_id]]) {
    float2 corners[4] = {
        float2(-1.0, -1.0),
        float2( 1.0, -1.0),
        float2(-1.0,  1.0),
        float2( 1.0,  1.0),
    };
    float2 c = corners[vid];
    VsOut o;
    o.position = float4(c, 0.0, 1.0);
    o.uv = float2(c.x * 0.5 + 0.5, 1.0 - (c.y * 0.5 + 0.5));
    return o;
}

// 1D hash → float in [0, 1)
static float hash1(float x) {
    return fract(sin(x) * 43758.5453123);
}

// 2D hash → float in [0, 1)
static float hash2(float2 p) {
    float h = dot(p, float2(127.1, 311.7));
    return fract(sin(h) * 43758.5453123);
}

// 3D hash → float in [0, 1).  Used for (cell, beat, seed) tuples.
static float hash3(float3 p) {
    float h = dot(p, float3(127.1, 311.7, 74.7));
    return fract(sin(h) * 43758.5453123);
}

// Symmetric variant returning -1..+1.
static float hash3s(float3 p) {
    return hash3(p) * 2.0 - 1.0;
}

fragment float4 glitch_fs(VsOut in                            [[stage_in]],
                          constant GlitchUniforms& u          [[buffer(0)]],
                          texture2d<float, access::sample> src [[texture(0)]],
                          sampler                          smp [[sampler(0)]])
{
    float A = u.amountBeat.x;          // effective amount
    if (A <= 1e-4) {
        return src.sample(smp, in.uv); // bypass
    }

    float beatIndex = u.amountBeat.z;  // integer beat (or frame if bpm=0)
    float seed      = u.paramsA.x;
    float bandPx    = max(u.paramsA.y, 1.0);
    float texelY    = u.paramsA.w;
    float texelX    = u.paramsA.z;

    float2 uv = in.uv;

    // ============================================================
    // 1. Displacement bands
    // Stride = bandPx in pixel space → bandPx * texelY in UV space.
    // Each band gets a random horizontal offset, gated by per-band
    // "active" probability that increases with amount.
    // ============================================================
    if (u.enables.x > 0.5 && A > u.thresholds.x) {
        float bandUv   = bandPx * texelY;
        float bandRow  = floor(uv.y / bandUv);
        // Hash on (bandRow, beatIndex, seed) → stable within a beat.
        float h        = hash3(float3(bandRow, beatIndex, seed));
        float active   = hash3(float3(bandRow + 31.0, beatIndex, seed));
        // Band fires only above its own random threshold scaled by A.
        if (active < A * 0.6 + 0.05) {
            // Center-around-zero offset proportional to amount.
            float ofs = (h * 2.0 - 1.0) * A * 0.20;
            uv.x += ofs;
        }
    }

    // ============================================================
    // 4. Tear line — one tall thin slice hard-shifted.
    // We pick a tear band per beat: hash determines its position,
    // its width, and its shift direction. Only applies when amount
    // crosses the tear threshold.
    // ============================================================
    if (u.enables.w > 0.5 && A > u.thresholds.w) {
        float tearY      = hash1(beatIndex * 7.31 + seed * 1.3);
        float tearWidth  = mix(0.005, 0.02, hash1(beatIndex * 11.7 + seed));
        float dy         = uv.y - tearY;
        if (abs(dy) < tearWidth) {
            float shift = (hash1(beatIndex * 5.1 + seed) > 0.5 ? +1.0 : -1.0)
                        * mix(0.05, 0.40, A);
            uv.x += shift;
            // Wrap so we don't sample outside [0,1] — Metal samplers
            // are set to "clamp to edge" by default; explicit fract gives
            // the classic VHS "wrap-around" look.
            uv.x = fract(uv.x + 1.0);
        }
    }

    // Sample base color with the (possibly displaced) UV.
    float4 baseColor = src.sample(smp, uv);

    // ============================================================
    // 2. RGB swap — random rectangular regions get R↔B swapped.
    // Tile the screen into ~16x9 cells, each with an independent
    // hash. A cell fires if its hash < (A * 0.7).
    // ============================================================
    if (u.enables.y > 0.5 && A > u.thresholds.y) {
        float2 cellSize = float2(1.0 / 16.0, 1.0 / 9.0);
        float2 cell     = floor(uv / cellSize);
        float  cellHash = hash3(float3(cell.x, cell.y * 17.0 + beatIndex, seed));
        if (cellHash < A * 0.7) {
            // Swap R↔B. The classic VHS "head misalignment" artifact.
            float r = baseColor.r;
            baseColor.r = baseColor.b;
            baseColor.b = r;
        }
    }

    // ============================================================
    // 3. Block dropout — random ~12x4 cells go black / static.
    // Coarser grid than RGB swap, lower fire rate (the artifact is
    // more visually disruptive).
    // ============================================================
    if (u.enables.z > 0.5 && A > u.thresholds.z) {
        float2 cellSize = float2(1.0 / 12.0, 1.0 / 4.0);
        float2 cell     = floor(uv / cellSize);
        float  cellHash = hash3(float3(cell.x + 53.0, cell.y * 23.0 + beatIndex, seed));
        if (cellHash < A * 0.30) {
            // Two flavours: pure black (low-amount), or hashed-noise
            // (high-amount). The boundary is at A=0.6.
            if (A > 0.6) {
                // Noise: per-fragment hash mapped to grayscale.
                float n = hash2(uv * 1000.0 + beatIndex);
                baseColor = float4(n, n, n, 1.0);
            } else {
                baseColor = float4(0.0, 0.0, 0.0, 1.0);
            }
        }
    }

    // ============================================================
    // 5. Color flash — full-screen pulse on the first frames of a beat.
    // We use beatPhase < 0.04 (≈ 1-2 frames at 60fps for fast beats)
    // gated by enable + threshold. Flash color is configurable.
    // ============================================================
    if (u.flashCtrl.x > 0.5 && A > u.flashCtrl.y) {
        float phase = u.amountBeat.y;        // 0..1 across the beat
        if (phase < 0.04) {
            // Hard mix to flash color, intensity scales with amount.
            float k = (1.0 - phase / 0.04) * A;
            baseColor.rgb = mix(baseColor.rgb, u.flashColor.rgb, k);
        }
    }

    return baseColor;
}
)MSL"
