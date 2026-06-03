// fracture.metal.inc — MSL snippets for MeshFractureLayer
//
// Drop this file into engine/backends/metal/shaders/ and #include it from
// the active structure shader (structure_pbr.metal or its successor) AFTER
// the Uniforms struct declaration and BEFORE vs_main / fs_main.
//
// All functions are pure (no side effects) and depend only on the
// FractureUniforms block — see INTEGRATION.md for the C++ side.
//
// Algorithm references:
//   - Voronoi cell + edge distance: Inigo Quilez, "Voronoi distances"
//     https://iquilezles.org/articles/voronoilines/
//   - 3D value noise hash: IQ "Useful little functions"
//   - Dissolve technique: standard since UE3; canonical write-up in
//     Ben Cloward's "Dissolve Shader" (2020).
//   - Voxel-grid shard partition: Müller et al. 2013,
//     "Real Time Dynamic Fracture with Volumetric Approximate Convex
//     Decompositions" — we use only the regular-grid bucketing trick.
//
// Conventions:
//   - All world-space math.
//   - Bit flags match MeshFractureLayer::Mode (see header).
//   - Time enters via u.time (seconds).
//   - `amount` is the resolved master amount in [0, 1].

#ifndef SPACEGEN_FRACTURE_INC
#define SPACEGEN_FRACTURE_INC

#include <metal_stdlib>
using namespace metal;

// ---- Bit flags (mirror MeshFractureLayer::Mode) -----------------------------
constant uint kFracModeCracks   = 0x1u;
constant uint kFracModeDissolve = 0x2u;
constant uint kFracModeExplode  = 0x4u;
constant uint kFracModeGlitch   = 0x8u;

// ---- Hashes -----------------------------------------------------------------
// Cheap, non-cryptographic hashes. Returns [0,1).
inline float fracHash1(float3 p) {
    float3 s = sin(p * float3(127.1, 311.7, 74.7));
    return fract(s.x * 17.0 + s.y * 0.13 + s.z * 73.0);
}

inline float3 fracHash3(float3 p) {
    // Per-channel independent hash. Cheap; not statistically perfect.
    return fract(sin(float3(
        dot(p, float3(127.1, 311.7,  74.7)),
        dot(p, float3(269.5, 183.3, 246.1)),
        dot(p, float3(113.5, 271.9, 124.6))
    )) * 43758.5453);
}

// ---- 3D value noise --------------------------------------------------------
// Trilinear-interpolated value noise on a unit grid. Returns [0,1].
inline float fracValueNoise(float3 p) {
    float3 i = floor(p);
    float3 f = fract(p);
    float3 w = f * f * (3.0 - 2.0 * f);     // smoothstep weights

    float n000 = fracHash1(i + float3(0,0,0));
    float n100 = fracHash1(i + float3(1,0,0));
    float n010 = fracHash1(i + float3(0,1,0));
    float n110 = fracHash1(i + float3(1,1,0));
    float n001 = fracHash1(i + float3(0,0,1));
    float n101 = fracHash1(i + float3(1,0,1));
    float n011 = fracHash1(i + float3(0,1,1));
    float n111 = fracHash1(i + float3(1,1,1));

    float x00 = mix(n000, n100, w.x);
    float x10 = mix(n010, n110, w.x);
    float x01 = mix(n001, n101, w.x);
    float x11 = mix(n011, n111, w.x);
    float y0  = mix(x00,  x10,  w.y);
    float y1  = mix(x01,  x11,  w.y);
    return mix(y0, y1, w.z);
}

// ---- Voronoi: F1 distance, F1 site, edge distance --------------------------
// One pass over the 27 surrounding cells. Implements IQ's distance-to-edge
// trick: each pair of nearby sites defines a half-plane through their
// midpoint, perpendicular to the site-to-site direction. The min of dot(p,
// half-plane normal) over all such pairs is the true distance to the cell
// boundary.
struct FracVoroResult {
    float  F1;          // distance to the nearest site
    float  edgeDist;    // distance to the nearest cell boundary (IQ trick)
    float3 siteF1;      // world-space position of the F1 cell site
    float3 cellF1;      // integer cell coordinates of the F1 cell
};

inline FracVoroResult fracVoronoi27(float3 wp,
                                    float  density,
                                    float  jitter,
                                    float  timeJitter)
{
    float3 p     = wp * density;
    float3 cell  = floor(p);

    FracVoroResult r;
    r.F1       = 1e9;
    r.siteF1   = float3(0.0);
    r.cellF1   = float3(0.0);

    // Pass 1: find F1 site.
    for (int z = -1; z <= 1; ++z) {
    for (int y = -1; y <= 1; ++y) {
    for (int x = -1; x <= 1; ++x) {
        float3 neigh = cell + float3(x, y, z);
        float3 jit   = (fracHash3(neigh) - 0.5) * jitter;
        // Time-drift the site slightly so the network breathes.
        jit += float3(
            sin(timeJitter + neigh.x),
            sin(timeJitter * 1.1 + neigh.y),
            sin(timeJitter * 0.9 + neigh.z)) * (0.15 * jitter);
        float3 site  = neigh + 0.5 + jit;
        float3 diff  = site - p;
        float  d     = length(diff);
        if (d < r.F1) {
            r.F1     = d;
            r.siteF1 = site;
            r.cellF1 = neigh;
        }
    }}}

    // Pass 2: edge distance — for every neighbor cell (skipping the F1 cell
    // itself), compute the distance from p to the perpendicular bisector
    // between p's F1 site and that neighbor's site.
    r.edgeDist = 1e9;
    for (int z = -1; z <= 1; ++z) {
    for (int y = -1; y <= 1; ++y) {
    for (int x = -1; x <= 1; ++x) {
        float3 neigh = cell + float3(x, y, z);
        float3 jit   = (fracHash3(neigh) - 0.5) * jitter;
        jit += float3(
            sin(timeJitter + neigh.x),
            sin(timeJitter * 1.1 + neigh.y),
            sin(timeJitter * 0.9 + neigh.z)) * (0.15 * jitter);
        float3 site  = neigh + 0.5 + jit;
        // Skip the F1 cell itself (its bisector with itself is degenerate).
        if (all(abs(site - r.siteF1) < 1e-4)) continue;
        float3 mid   = 0.5 * (r.siteF1 + site);
        float3 dir   = normalize(site - r.siteF1);
        float  d     = dot(p - mid, dir);
        // d > 0 means p is on this neighbor's side, which can only happen
        // due to floating-point at jittered cell boundaries; clamp to 0.
        r.edgeDist   = min(r.edgeDist, abs(d));
    }}}

    // Convert F1 / edgeDist from "density-units" back to world-meters.
    r.F1       /= max(density, 1e-4);
    r.edgeDist /= max(density, 1e-4);
    r.siteF1   /= max(density, 1e-4);
    return r;
}

// ---- Mode 1: Cracks (fragment) ---------------------------------------------
// Returns:
//   .x = darkening multiplier in [0,1] (1 = no effect, 0 = full darken)
//   .yzw = additive emit color
inline float4 fracEvalCracks(float3 worldPos,
                             float  amount,
                             float  density,
                             float  width,
                             float  jitter,
                             float  jitterSpeed,
                             float  darkenStrength,
                             float3 glowColor,
                             float  glow,
                             float  t)
{
    FracVoroResult v = fracVoronoi27(worldPos, density, jitter, t * jitterSpeed);
    // edgeDist is in meters; `width` is interpreted in meters too.
    float c = 1.0 - smoothstep(0.0, width, v.edgeDist);
    c *= amount;
    float darken = 1.0 - c * darkenStrength;
    float3 emit  = glowColor * (c * glow);
    return float4(darken, emit);
}

// ---- Mode 2: Dissolve (fragment) -------------------------------------------
// Returns:
//   .x  = keep (1 = render, 0 = discard)
//   .y  = edgeGlow scalar (0..1) for additive emit
//   .zw = unused
//
// `useRadial != 0` → threshold modulated by distance to burnPoint.
inline float4 fracEvalDissolve(float3 worldPos,
                               float  amount,
                               float  noiseScale,
                               float3 burnPoint,
                               float  burnRadius,
                               float  radialBias,
                               int    useRadial,
                               float  edgeWidth,
                               float  speed,
                               float  t)
{
    // Per-fragment noise in [0,1].
    float n = fracValueNoise(worldPos * noiseScale);

    // Threshold animates from `amount` and `speed*t`. Radial mode warps it.
    float a = saturate(amount + speed * t);
    float thr;
    if (useRadial != 0) {
        float r = length(worldPos - burnPoint) / max(burnRadius, 1e-3);
        // r ∈ [0,∞). Inside the burn radius we want a higher threshold so
        // those fragments dissolve first; outside we want a lower threshold.
        thr = saturate(a - (r - 0.5) * radialBias);
    } else {
        thr = a;
    }

    // keep == 1 when noise > 1 - thr (i.e. the lower part of the noise gets
    // burned away as thr grows).
    float keep      = step(n, 1.0 - thr);                  // inverted: 1 = burned
    float renderKeep = 1.0 - keep;                         // 1 = render
    float edge      = 1.0 - smoothstep(0.0, edgeWidth, abs(n - (1.0 - thr)));
    return float4(renderKeep, edge, 0.0, 0.0);
}

// ---- Mode 3: Explode (vertex) ----------------------------------------------
// Returns the world-space offset (in meters) to add to the vertex BEFORE the
// projection*view multiplication. `structureCentroid` lets shards explode
// outward from the geometric center; pass scene.centroid from the C++ side.
inline float3 fracEvalExplode(float3 worldPos,
                              float3 structureCentroid,
                              float  amount,
                              float  density,
                              float  jitter,
                              float  spin,
                              float  strength)
{
    float3 p        = worldPos * density;
    float3 cell     = floor(p);
    float3 cellCntr = (cell + 0.5) / max(density, 1e-4);
    float3 h        = fracHash3(cell);   // per-shard random vec3 in [0,1)

    // Direction: from structure centroid to cell center, plus per-shard jitter.
    float3 toCell = cellCntr - structureCentroid;
    float  lenC   = length(toCell);
    float3 outDir = (lenC > 1e-4) ? (toCell / lenC) : float3(0.0, 0.0, 1.0);
    float3 jit    = (h - 0.5) * 2.0 * jitter;
    float3 dir    = normalize(outDir + jit);

    // Per-shard magnitude: 0.5..1.0 of strength, scaled by amount.
    float mag = strength * amount * (0.5 + 0.5 * h.x);
    float3 offset = dir * mag;

    // Spin: rotate the vertex around the cell center by an angle proportional
    // to amount * spin * per-shard random. Skip when spin == 0 to save ALU.
    if (spin > 1e-4) {
        float angle = amount * spin * (h.y - 0.5) * 6.2831853;
        // Axis from hash, normalized; ensure non-degenerate.
        float3 axis = normalize(h * 2.0 - 1.0 + float3(0.001, 0.0, 0.0));
        float ca = cos(angle), sa = sin(angle);
        // Rodrigues' rotation formula applied to (worldPos - cellCntr).
        float3 rel  = worldPos - cellCntr;
        float3 rot  = rel * ca
                    + cross(axis, rel) * sa
                    + axis * dot(axis, rel) * (1.0 - ca);
        offset += (cellCntr + rot) - worldPos;
    }

    return offset;
}

// ---- Mode 4: Glitch tear (vertex) ------------------------------------------
// Vertex displacement only. Per-fragment alpha flicker is handled in the
// fragment dispatcher below.
inline float3 fracEvalGlitchVertex(float3 worldPos,
                                   float  amount,
                                   float  frequency,
                                   float  magnitude,
                                   float  speed,
                                   float  t)
{
    float band  = floor(worldPos.y * frequency + 0.5);
    float frame = floor(t * speed);
    float h     = fracHash1(float3(band, frame, 0.0));
    float roll  = fracHash1(float3(band, frame, 17.0));
    // Only the top `amount` fraction of bands actually tear, so at low amount
    // you get a few stray slips rather than the whole mesh shearing.
    float tear  = step(1.0 - amount, h);
    float3 offset = float3(0.0);
    offset.x = tear * (roll - 0.5) * 2.0 * magnitude * amount;
    return offset;
}

// Glitch alpha (fragment-side). Returns a [0,1] multiplier to apply to the
// final alpha (and optionally to the RGB if you want flicker-darken instead
// of pure transparency).
inline float fracEvalGlitchAlpha(float3 worldPos,
                                 float  amount,
                                 float  frequency,
                                 float  speed,
                                 float  dropout,
                                 float  t)
{
    float band  = floor(worldPos.y * frequency + 0.5);
    float frame = floor(t * speed);
    float h     = fracHash1(float3(band, frame, 97.0));
    // h < dropout * amount  → flicker
    float flicker = step(h, dropout * amount);
    return 1.0 - flicker;
}

// ---- Vertex dispatcher -----------------------------------------------------
// Call from vs_main AFTER twist/displace have been applied to `pos` (still in
// local space), and AFTER worldPos = (u.model * float4(pos, 1)).xyz has been
// computed. Returns the world-space offset to ADD to the current world
// position before the projection*view multiplication.
inline float3 fracVertexOffset(uint    fracMode,
                               float   amount,
                               float3  worldPos,
                               float3  structureCentroid,
                               // explode
                               float   explodeDensity,
                               float   explodeJitter,
                               float   explodeSpin,
                               float   explodeStrength,
                               // glitch
                               float   glitchFrequency,
                               float   glitchMagnitude,
                               float   glitchSpeed,
                               float   t)
{
    float3 off = float3(0.0);
    if ((fracMode & kFracModeExplode) != 0) {
        off += fracEvalExplode(worldPos,
                               structureCentroid,
                               amount,
                               explodeDensity,
                               explodeJitter,
                               explodeSpin,
                               explodeStrength);
    }
    if ((fracMode & kFracModeGlitch) != 0) {
        off += fracEvalGlitchVertex(worldPos,
                                    amount,
                                    glitchFrequency,
                                    glitchMagnitude,
                                    glitchSpeed,
                                    t);
    }
    return off;
}

// ---- Fragment dispatcher ---------------------------------------------------
// `baseRGB` is the lit color before fracture (after PBR + ambient + tone-map
// candidates). Returns the fractured color and writes through an alpha
// multiplier. The caller is responsible for `discard_fragment()` when
// outAlphaMul drops to 0 (we don't call discard here so the shader can choose
// alpha-blend instead).
inline float3 fracFragmentApply(uint    fracMode,
                                float   amount,
                                float3  worldPos,
                                float3  baseRGB,
                                thread float& outAlphaMul,
                                // cracks
                                float   crackDensity,
                                float   crackWidth,
                                float   crackJitter,
                                float   crackJitterSpeed,
                                float   crackDarken,
                                float3  crackGlowColor,
                                float   crackGlow,
                                // dissolve
                                float   dissolveScale,
                                float3  burnPoint,
                                float   burnRadius,
                                float   radialBias,
                                int     useRadial,
                                float   edgeWidth,
                                float   dissolveSpeed,
                                float3  dissolveEdgeColor,
                                float   dissolveEdgeGlow,
                                // glitch
                                float   glitchFrequency,
                                float   glitchSpeed,
                                float   glitchDropout,
                                float   t)
{
    float3 rgb     = baseRGB;
    float  alphaM  = 1.0;

    if ((fracMode & kFracModeCracks) != 0) {
        float4 cr = fracEvalCracks(worldPos, amount,
                                   crackDensity, crackWidth,
                                   crackJitter, crackJitterSpeed,
                                   crackDarken, crackGlowColor, crackGlow,
                                   t);
        rgb = rgb * cr.x + cr.yzw;
    }

    if ((fracMode & kFracModeDissolve) != 0) {
        float4 dr = fracEvalDissolve(worldPos, amount,
                                     dissolveScale,
                                     burnPoint, burnRadius,
                                     radialBias, useRadial,
                                     edgeWidth, dissolveSpeed,
                                     t);
        alphaM *= dr.x;
        rgb    += dissolveEdgeColor * (dr.y * dissolveEdgeGlow);
    }

    if ((fracMode & kFracModeGlitch) != 0) {
        alphaM *= fracEvalGlitchAlpha(worldPos, amount,
                                      glitchFrequency, glitchSpeed,
                                      glitchDropout, t);
    }

    outAlphaMul = alphaM;
    return rgb;
}

#endif // SPACEGEN_FRACTURE_INC
