// hologram.metal.inc -- MSL fragment-side hologram material.
//
// This file is conceptually a header that lives inside kStructurePbrMSL.
// At integration time the maintainer pastes the blocks marked
// "[BEGIN ...]" / "[END ...]" into MetalRenderer.cpp at the insertion
// points described in INTEGRATION.md.
//
// The shader exposes seven sub-effects controlled by a single uint
// `effectMask`. Disabled sub-effects compile-time-collapse because the
// mask test is a uniform-conditional branch.
//
// Author note on Metal: this file is NOT a separate Metal library. We
// inline it into the existing structure shader so we keep one pipeline
// (and one draw call) instead of adding a separate hologram pass.
// =============================================================================

// [BEGIN HOLO_UNIFORMS]
// Append to the existing `Uniforms` struct in kStructurePbrMSL.

constant uint HMASK_ENABLE           = 1u << 0;
constant uint HMASK_REPLACE          = 1u << 1;
constant uint HMASK_SCAN             = 1u << 2;
constant uint HMASK_SCAN_HARMONIC    = 1u << 3;
constant uint HMASK_FRESNEL          = 1u << 4;
constant uint HMASK_FLICKER          = 1u << 5;
constant uint HMASK_GLITCH           = 1u << 6;
constant uint HMASK_GLITCH_RGB_SHIFT = 1u << 7;
constant uint HMASK_WIRE_DERIV       = 1u << 8;
constant uint HMASK_WIRE_BARY        = 1u << 9;
constant uint HMASK_HUE_SHIFT        = 1u << 10;
constant uint HMASK_CHROMA_ABER      = 1u << 11;
constant uint HMASK_DISSOLVE         = 1u << 12;
constant uint HMASK_DISSOLVE_NOISE   = 1u << 13;

constant float HOLO_TAU = 6.28318530718;

// Packed into Uniforms.holo[0..7] (8 float4s = 128 B).
//   holo[0] = (effectMask_asFloat, opacity, elapsedSeconds, viewportH)
//   holo[1] = (scanFreq, scanSpeed, scanIntensity, _)
//   holo[2] = (fresnelColor.rgb, fresnelPower)
//   holo[3] = (fresnelIntensity, flickerRate, flickerProb, flickerDuration)
//   holo[4] = (flickerColor.rgb, _)
//   holo[5] = (glitchRate, glitchBands, glitchProb, glitchAmplitude)
//   holo[6] = (wireColor.rgb, wireThickness)
//   holo[7] = (wireSharpness, wireIntensity, hueSpeed, hueOffset)
//   holo[8] = (caAmount, dissolveOrigin, dissolveSpeed, dissolveBand)
//   holo[9] = (dissolveAxis_asFloat, dissolveNoiseAmp,
//             masterGlowColor.rg)
//   holo[10] = (masterGlowColor.b, 0, 0, 0)
//
// NOTE: 11 float4s = 176 B. Keep this contiguous in the larger Uniforms
// block so the struct stays 16-byte aligned. See INTEGRATION.md for the
// exact CPU mirror.
// [END HOLO_UNIFORMS]


// [BEGIN HOLO_HELPERS]
// Place between the PBR helpers and `fs_main` in kStructurePbrMSL.

static float holo_hash11(float x) {
    return fract(sin(x * 127.1) * 43758.5453);
}

static float holo_hash21(float2 p) {
    return fract(sin(dot(p, float2(127.1, 311.7))) * 43758.5453);
}

// Luminance-preserving YIQ hue rotation. Single matrix-multiply, no
// RGB↔HSV round-trip. Reference: Graphics Gems, "RGB Rotation Matrix".
static float3 holo_hueRot(float3 c, float angle) {
    float cs = cos(angle);
    float sn = sin(angle);
    float3x3 m = float3x3(
        float3(0.299 + 0.701 * cs + 0.168 * sn,
               0.587 - 0.587 * cs + 0.330 * sn,
               0.114 - 0.114 * cs - 0.497 * sn),
        float3(0.299 - 0.299 * cs - 0.328 * sn,
               0.587 + 0.413 * cs + 0.035 * sn,
               0.114 - 0.114 * cs + 0.292 * sn),
        float3(0.299 - 0.300 * cs + 1.250 * sn,
               0.587 - 0.588 * cs - 1.050 * sn,
               0.114 + 0.886 * cs - 0.203 * sn));
    return m * c;
}

// Screen-space U shift for the active glitch band (if any). 0 if no
// band is active at this fragment / time, otherwise a signed offset in
// screen-normalized units. Caller multiplies through to its sample UVs.
static float holo_glitchShift(uint mask,
                              float screenY01,
                              float elapsed,
                              float4 g /* rate, bands, prob, amp */)
{
    if ((mask & HMASK_GLITCH) == 0u) return 0.0;
    float bucket  = floor(elapsed * g.x);
    float band    = floor(screenY01 * g.y);
    float hActive = holo_hash21(float2(band, bucket));
    float hOffset = holo_hash21(float2(band, bucket + 91.0)) * 2.0 - 1.0;
    float active  = step(1.0 - g.z, hActive);
    return hOffset * g.w * active;
}

// Edge-distance metric for derivative-based wireframe. Returns a
// value in [0..1]; 0 at triangle centers, → 1 at triangle edges.
// Uses world-position derivative discontinuity, so it works on any
// indexed mesh without baked barycentrics. Quality is lower than
// true bary wireframe (especially at glancing angles) but adequate
// for the "hologram" aesthetic.
static float holo_edgeDeriv(float3 worldPos) {
    float3 dPx  = dfdx(worldPos);
    float3 dPy  = dfdy(worldPos);
    float3 nrm  = cross(dPx, dPy);
    // Per-fragment "triangle area gradient". Crude but stable.
    float area  = length(nrm);
    float fw    = fwidth(area);
    return saturate(fw / max(area, 1e-4));
}

// True barycentric wireframe (Forsyth 2007). Requires baked-per-vertex
// barycentric coords in VertexOut. `bary` is the interpolated (b0,b1,b2).
static float holo_edgeBary(float3 bary, float thickness, float sharpness) {
    float3 deltas = fwidth(bary);
    float3 smoothB = smoothstep(float3(0.0),
                                deltas * thickness,
                                bary);
    float edge = 1.0 - min(min(smoothB.x, smoothB.y), smoothB.z);
    return pow(edge, sharpness);
}
// [END HOLO_HELPERS]


// [BEGIN HOLO_EARLY_SHIFT]
// Place at the TOP of fs_main, immediately after `float3 V = normalize(...)`.
// This computes the glitch UV shift once and applies it to in.uv / in.uv1
// before any texture sampling.

uint  holoMask     = uint(u.holo[0].x + 0.5);
float holoOpacity  = u.holo[0].y;
float holoElapsed  = u.holo[0].z;
float holoVpH      = u.holo[0].w;
bool  holoOn       = (holoMask & HMASK_ENABLE) != 0u;

float holoScreenY  = saturate(in.position.y / max(holoVpH, 1.0));
float holoGlitchU  = holoOn
    ? holo_glitchShift(holoMask, holoScreenY, holoElapsed, u.holo[5])
    : 0.0;

if (holoOn && abs(holoGlitchU) > 1e-6) {
    // Apply the shift to both UVs so the syphon projection and the
    // material texture shear together. The glitch only displaces by
    // U (horizontal); V is untouched.
    in.uv.x  += holoGlitchU;
    in.uv1.x += holoGlitchU;
}
// [END HOLO_EARLY_SHIFT]


// [BEGIN HOLO_POST_PBR]
// Place at the END of fs_main, immediately BEFORE the final
// `return float4(colorOut, 1.0);` (or before the lightsOnly return path —
// the operator must pick one; see INTEGRATION.md).
//
// At this point `colorOut` is the PBR-shaded sRGB-encoded color, and we
// have access to `colorLin` (linear PBR) plus `N`, `V`, `in.worldPos`.

if (holoOn) {
    float3 holoLin = colorLin;                 // start from linear PBR
    bool   replace = (holoMask & HMASK_REPLACE) != 0u;
    if (replace) {
        // Pure-replace: hologram only; PBR base color & lighting suppressed.
        // We keep emission so light-emitting materials still glow through.
        holoLin = emission;
    }

    // ---- 6a. Hue rotation (BEFORE scanlines so they don't get re-tinted)
    if ((holoMask & HMASK_HUE_SHIFT) != 0u) {
        float ang = u.holo[7].z * holoElapsed + u.holo[7].w;
        holoLin   = holo_hueRot(holoLin, ang);
    }

    // ---- 1. Scan lines (multiplicative)
    if ((holoMask & HMASK_SCAN) != 0u) {
        float freq  = u.holo[1].x;
        float speed = u.holo[1].y;
        float depth = u.holo[1].z;
        float phase = holoScreenY * freq - holoElapsed * speed;
        float wave  = 0.5 + 0.5 * sin(phase * HOLO_TAU);
        if ((holoMask & HMASK_SCAN_HARMONIC) != 0u) {
            wave += 0.25 * sin(phase * 3.0 * HOLO_TAU);
            wave  = saturate(wave);
        }
        float mask = mix(1.0 - depth, 1.0, wave);
        holoLin   *= mask;
    }

    // Emission accumulator for the additive elements (Fresnel + wire).
    float3 holoEmissive = float3(0.0);
    float3 masterGlow   = float3(u.holo[9].z, u.holo[9].w, u.holo[10].x);

    // ---- 2. Fresnel rim
    float fresnel = 0.0;
    if ((holoMask & HMASK_FRESNEL) != 0u) {
        float3 fcol = u.holo[2].rgb * masterGlow;
        float  pwr  = max(u.holo[2].w, 0.0001);
        float  intn = u.holo[3].x;
        float  NdotV = saturate(dot(N, V));
        fresnel = pow(1.0 - NdotV, pwr);
        float3 rim = fcol * fresnel * intn;

        // ---- 3. Edge flicker (modulates the rim)
        if ((holoMask & HMASK_FLICKER) != 0u) {
            float rate = u.holo[3].y;
            float prob = u.holo[3].z;
            float dur  = max(u.holo[3].w, 0.01);
            float bf   = holoElapsed * rate;
            float bk   = floor(bf);
            float local = bf - bk;
            float h    = holo_hash11(bk + 13.37);
            float trig = step(1.0 - prob, h);
            float env  = trig * (1.0 - smoothstep(0.0, dur, local));
            float3 fcCol = u.holo[4].rgb;
            rim = mix(rim, fcCol * intn * 3.0, env);
        }
        holoEmissive += rim;
    }

    // ---- 5. Wireframe (additive)
    if ((holoMask & HMASK_WIRE_BARY) != 0u) {
        // Requires VertexOut.bary; see INTEGRATION.md.
        float thick = u.holo[6].w;
        float sharp = u.holo[7].x;
        float edge  = holo_edgeBary(in.bary, thick, sharp);
        holoEmissive += u.holo[6].rgb * masterGlow * edge * u.holo[7].y;
    } else if ((holoMask & HMASK_WIRE_DERIV) != 0u) {
        float edge = holo_edgeDeriv(in.worldPos);
        edge = pow(edge, u.holo[7].x);
        holoEmissive += u.holo[6].rgb * masterGlow * edge * u.holo[7].y;
    }

    holoLin += holoEmissive;

    // ---- 7. Z-fade dissolve (final multiplicative fade)
    float holoFade = 1.0;
    if ((holoMask & HMASK_DISSOLVE) != 0u) {
        int  axis   = int(u.holo[9].x + 0.5);
        float origin = u.holo[8].y;
        float speed  = u.holo[8].z;
        float band   = max(u.holo[8].w, 1e-4);
        float coord  = (axis == 0) ? in.worldPos.x
                     : (axis == 1) ? in.worldPos.y
                                   : in.worldPos.z;
        float plane  = origin + speed * holoElapsed;
        float t      = (coord - plane) / band;
        if ((holoMask & HMASK_DISSOLVE_NOISE) != 0u) {
            float n = holo_hash21(in.worldPos.xy * 7.0) * 2.0 - 1.0;
            t += n * u.holo[9].y;
        }
        holoFade = 1.0 - saturate(t);
    }
    holoLin *= holoFade;

    // ---- Cross-fade with original PBR result via master opacity.
    float blend = saturate(holoOpacity * holoFade);
    float3 mixed = replace
        ? holoLin                            // pure replace: just use hologram
        : mix(colorLin, holoLin, blend);     // overlay mode: cross-fade

    // Re-encode and update colorOut so the existing return path uses our
    // value. The original "lightsOnly" return path is taken EARLIER and
    // does not pass through here — see INTEGRATION.md for the case where
    // both emitLightsOnly and the hologram are simultaneously enabled.
    colorOut = pow(max(mixed, 0.0), float3(1.0 / 2.2));
}
// [END HOLO_POST_PBR]


// [BEGIN HOLO_CHROMA_SYPHON]
// OPTIONAL: chroma-aberration on the syphon sample. Place this inside the
// existing `if (syphonMix > 1e-4) { ... }` block, replacing the single
// `atlasSample = syphonMap.sample(smp, atlasUv).rgb;` line.

if (holoOn && (holoMask & HMASK_CHROMA_ABER) != 0u) {
    float ca = u.holo[8].x;
    float r = syphonMap.sample(smp, atlasUv + float2(ca, 0.0)).r;
    float g = syphonMap.sample(smp, atlasUv                  ).g;
    float b = syphonMap.sample(smp, atlasUv - float2(ca, 0.0)).b;
    atlasSample = float3(r, g, b);
} else {
    atlasSample = syphonMap.sample(smp, atlasUv).rgb;
}
// [END HOLO_CHROMA_SYPHON]


// [BEGIN HOLO_VS_BARY]
// OPTIONAL: baked-barycentric wireframe (HMASK_WIRE_BARY). Requires a new
// vertex attribute on the structure mesh and a matching VertexOut field.
//
// Vertex input change:
//   struct VertexIn {
//     ...existing attributes...
//     float3 bary [[attribute(4)]];   // baked (1,0,0)/(0,1,0)/(0,0,1)
//   };
//
// VertexOut change:
//   struct VertexOut {
//     ...existing fields...
//     float3 bary;
//   };
//
// vs_main passthrough (just after `out.uv1 = in.uv1;`):
//   out.bary = in.bary;
//
// CPU-side: MetalRenderer::buildBarycentricBuffer(meshIdx) builds an
// unindexed copy of the mesh with per-vertex (1,0,0)/(0,1,0)/(0,0,1)
// triplets. This 3× vertex inflation is the well-known cost of GS-free
// wireframe; only pay it for meshes that actually want it.
// [END HOLO_VS_BARY]
