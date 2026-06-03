// =============================================================================
// ProceduralMaterial — MSL pattern library
// =============================================================================
//
// 10 per-fragment procedural texture patterns + a switch-based dispatcher.
// All ten ship in a single MSL compile inside MetalRenderer's
// `kStructurePbrMSL` string. See INTEGRATION.md for the exact insertion
// point and the uniform layout.
//
// References:
//   - Worley, "A Cellular Texture Basis Function", SIGGRAPH 1996
//   - Bridson, "Curl-Noise for Procedural Fluid Flow", SIGGRAPH 2007
//   - Perlin, "Improving Noise", SIGGRAPH 2002
//   - Inigo Quilez, iquilezles.org/articles/{voronoilines, fbm, hexagons,
//                                              warp, palettes}
//
// Conventions:
//   - All pattern fns take (float2 p, float time, ...) and return float3.
//   - `p` is already scaled by `u.proc.shape.x` (scale) at the dispatcher
//     site, and animation offsets are already added — pattern fns are
//     pure of `time` only where they need internal animation (Voronoi
//     feature jitter, brick flicker).
//   - Palette colour A / B come in via the proc uniform block and are
//     resolved with `procPalette(t, ...)`.
//
// Insert this whole file VERBATIM into the MSL raw string in
// MetalRenderer.cpp, between the existing PBR helpers
// (geometrySmith / pbrEvalDirect) and the start of `fs_main`. Wrap it in
// an `#if 0 ... #endif` toggle if you ever need to disable the feature
// without removing the code.
// -----------------------------------------------------------------------------

// ============================================================================
// Hash primitives — IQ's `iqint` style sine-mantissa hashes.
// ============================================================================

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

// ============================================================================
// Value noise + 2D / 3D fbm.  IQ canon, `iquilezles.org/articles/fbm`.
// ============================================================================

static float procValueNoise2(float2 p) {
    float2 i = floor(p);
    float2 f = fract(p);
    // Quintic smoothstep — perlin's improved noise (2002).
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
    float v = 0.0;
    float amp = 0.5;
    // 2x2 rotation between octaves — IQ's trick to reduce axis-aligned
    // artefacts in fbm.
    float2x2 rot = float2x2(0.8, 0.6, -0.6, 0.8);
    int n = clamp(octaves, 1, 8);
    for (int i = 0; i < n; ++i) {
        v += amp * procValueNoise2(p);
        p = rot * p * 2.0;
        amp *= 0.5;
    }
    return v;
}

static float procFbm3(float3 p, int octaves) {
    float v = 0.0;
    float amp = 0.5;
    int n = clamp(octaves, 1, 8);
    for (int i = 0; i < n; ++i) {
        v += amp * procValueNoise3(p);
        p *= 2.0;
        amp *= 0.5;
    }
    return v;
}

// ============================================================================
// Palette — two-colour cosine-smoothed blend.
// ============================================================================

static float3 procPalette(float t, float3 A, float3 B) {
    t = clamp(t, 0.0, 1.0);
    // Smoothstep is the cheapest perceptually-pleasant ramp; cosine
    // works too but smoothstep is one fewer transcendental per pixel.
    float s = smoothstep(0.0, 1.0, t);
    return mix(A, B, s);
}

// Shape a [0..1] value by contrast around 0.5. contrast == 1.0 → identity,
// contrast == 2.0 → hard threshold, contrast == 0.0 → flat 0.5.
static float procApplyContrast(float t, float c) {
    float s = clamp(c, 0.0, 2.0);
    return clamp((t - 0.5) * s + 0.5, 0.0, 1.0);
}

// ============================================================================
// Pattern 1 — Voronoi cells (Worley F1).
// ============================================================================
//
// Worley 1996; IQ's 3×3 neighbourhood scan
// (iquilezles.org/articles/voronoilines).
//
// Modes:
//   contrast < 0.5 → filled cells (distance ramp, dark centres)
//   contrast ≥ 0.5 → crack lines  (smooth threshold of F1)

static float3 procVoronoi(float2 p, float time, float contrast,
                          float3 A, float3 B) {
    float2 i = floor(p);
    float2 f = fract(p);
    float minD = 1e9;
    for (int y = -1; y <= 1; ++y)
    for (int x = -1; x <= 1; ++x) {
        float2 g = float2(float(x), float(y));
        float2 o = procHash22(i + g);
        // Animate feature points around their cell centres.
        o = 0.5 + 0.5 * sin(time + 6.2831 * o);
        float2 r = g + o - f;
        minD = min(minD, dot(r, r));
    }
    float F1 = sqrt(minD);
    if (contrast < 0.5) {
        // Filled cells. Brighter at centre, darker at edges.
        float t = procApplyContrast(1.0 - F1, contrast * 2.0);
        return procPalette(t, A, B);
    } else {
        // Crack lines. fwidth-driven AA.
        float edge = 0.04 + 0.16 * (1.0 - (contrast - 0.5) * 2.0);
        float aa   = fwidth(F1) * 1.5 + 1e-4;
        float k    = smoothstep(edge, edge + aa, F1);
        return procPalette(1.0 - k, A, B);
    }
}

// ============================================================================
// Pattern 2 — Perlin fbm.
// ============================================================================

static float3 procPerlinFbm(float2 p, float time, float contrast,
                             int octaves, float3 A, float3 B) {
    // Time-scrolled fbm. 3D sampling lets us evolve the field in z
    // smoothly, which looks better than translating in xy alone.
    float t = procFbm3(float3(p, time * 0.5), octaves);
    t = procApplyContrast(t, contrast);
    return procPalette(t, A, B);
}

// ============================================================================
// Pattern 3 — Curl noise (Bridson 2007).
// ============================================================================
//
// 2D curl of a potential field ψ is (∂ψ/∂y, -∂ψ/∂x). We pick ψ =
// fbm3(p, t) and central-difference the partials. The magnitude of the
// resulting velocity field has visible fluid-streamline texture.

static float3 procCurlNoise(float2 p, float time, float contrast,
                             int octaves, float3 A, float3 B) {
    const float eps = 0.04;
    float psi_xp = procFbm3(float3(p + float2(eps, 0.0), time * 0.4), octaves);
    float psi_xm = procFbm3(float3(p - float2(eps, 0.0), time * 0.4), octaves);
    float psi_yp = procFbm3(float3(p + float2(0.0, eps), time * 0.4), octaves);
    float psi_ym = procFbm3(float3(p - float2(0.0, eps), time * 0.4), octaves);
    float dpsi_dx = (psi_xp - psi_xm) / (2.0 * eps);
    float dpsi_dy = (psi_yp - psi_ym) / (2.0 * eps);
    float2 v = float2(dpsi_dy, -dpsi_dx);

    // |v| has expected magnitude ≈ 0.5..2 in this regime; remap to [0,1].
    float t = saturate(length(v) * 0.5);
    t = procApplyContrast(t, contrast);
    return procPalette(t, A, B);
}

// ============================================================================
// Pattern 4 — Hexagonal grid.
// ============================================================================
//
// IQ's hex tiling, iquilezles.org/articles/hexagons. Returns the
// distance-to-edge metric; we smoothstep it for AA cells.

static float procHexDist(float2 p) {
    // Two interleaved square lattices form the hex grid.
    p.x *= 1.1547005;          // 2 / sqrt(3)
    p.y += 0.5 * floor(fmod(p.x, 2.0));
    p = abs(fract(p) - 0.5);
    float d = abs(max(p.x * 1.5 + p.y, p.y * 2.0) - 1.0);
    return d;
}

static float3 procHex(float2 p, float time, float contrast,
                      float3 A, float3 B) {
    // Drift the lattice slowly so the cells "breathe".
    p += 0.05 * float2(cos(time * 0.7), sin(time * 0.5));
    float d  = procHexDist(p);
    float aa = fwidth(d) * 1.5 + 1e-4;
    // contrast remaps the edge threshold: high contrast = thin bright
    // edges; low contrast = wide soft edges.
    float thresh = 0.04 + 0.10 * (2.0 - contrast);
    float k = smoothstep(thresh, thresh + aa, d);
    return procPalette(1.0 - k, A, B);
}

// ============================================================================
// Pattern 5 — Multi-scale checker.
// ============================================================================
//
// Sum of layered signed-sin checkers normalised to [0,1]. Naturally
// anti-aliases at distance because higher-octave layers cancel out
// under derivative-driven mip selection.

static float3 procChecker(float2 p, float time, float contrast,
                           int octaves, float3 A, float3 B) {
    float c   = 0.0;
    float amp = 1.0;
    float wsum = 0.0;
    float2 q  = p * 3.1415926;       // π scaling — checker period = 1 uv
    q += time * 0.5;
    int n = clamp(octaves, 1, 8);
    for (int i = 0; i < n; ++i) {
        c += amp * sign(sin(q.x) * sin(q.y));
        wsum += amp;
        q *= 2.0;
        amp *= 0.5;
    }
    float t = (c / max(wsum, 1e-4)) * 0.5 + 0.5;
    t = procApplyContrast(t, contrast);
    return procPalette(t, A, B);
}

// ============================================================================
// Pattern 6 — Concentric rings.
// ============================================================================

static float3 procRings(float2 p, float time, float contrast,
                         float3 A, float3 B) {
    float d = length(p);
    float t = fract(d - time * 0.5);
    // Contrast widens or narrows the bright band.
    float w = 0.5 - 0.45 * (contrast - 1.0);
    w = clamp(w, 0.05, 0.95);
    float band = smoothstep(0.5 - w * 0.5, 0.5, t)
               * (1.0 - smoothstep(0.5, 0.5 + w * 0.5, t));
    return procPalette(band, A, B);
}

// ============================================================================
// Pattern 7 — Voronoi shatter (F2 - F1 gaps).
// ============================================================================
//
// Computes F1 and F2 in the same 3×3 scan. The edge `F2 - F1` is small
// near cell boundaries and large in cell interiors, so smoothstep gives
// us "gap mortar" between unique per-cell colours.

static float3 procVoronoiShatter(float2 p, float time, float contrast,
                                  float3 A, float3 B) {
    float2 i = floor(p);
    float2 f = fract(p);
    float F1 = 1e9, F2 = 1e9;
    float2 winnerCell = float2(0.0);
    for (int y = -1; y <= 1; ++y)
    for (int x = -1; x <= 1; ++x) {
        float2 g  = float2(float(x), float(y));
        float2 o  = procHash22(i + g);
        o = 0.5 + 0.5 * sin(time * 0.6 + 6.2831 * o);
        float2 r  = g + o - f;
        float d2  = dot(r, r);
        if (d2 < F1) {
            F2 = F1;
            F1 = d2;
            winnerCell = i + g;
        } else if (d2 < F2) {
            F2 = d2;
        }
    }
    float edge   = sqrt(F2) - sqrt(F1);
    float gapW   = 0.02 + 0.18 * (2.0 - contrast);
    float gap    = smoothstep(0.0, gapW, edge);

    // Per-cell colour: hash winning feature into [0..1], pick palette.
    float cellId = procHash21(winnerCell);
    float3 inside = procPalette(cellId, A, B);
    // Mortar (gap) gets the cooler colour of the two, halved.
    float3 mortar = B * 0.25;
    return mix(mortar, inside, gap);
}

// ============================================================================
// Pattern 8 — Wood grain.
// ============================================================================
//
// Elliptical rings + fbm radial perturbation, then thresholded into
// grain bands. The 0.1 / 1.0 scaling on q.x / q.y stretches isolines
// into elongated plank-grain shapes.

static float3 procWood(float2 p, float time, float contrast,
                       int octaves, float3 A, float3 B) {
    float2 q = p;
    float r = sqrt(q.x * q.x * 0.1 + q.y * q.y);
    r += 0.55 * procFbm3(float3(q * 3.0, time * 0.2), octaves);
    float g = fract(r);
    // Contrast controls grain band sharpness.
    float w = 0.5 - 0.45 * (contrast - 1.0);
    w = clamp(w, 0.05, 0.95);
    g = smoothstep(0.5 - w * 0.5, 0.5 + w * 0.5, g);
    return procPalette(g, A, B);
}

// ============================================================================
// Pattern 9 — Marble.
// ============================================================================
//
// Perlin's 1985 classic. sin(x + k·fbm) gives wandering veins; we use
// fbm with a small time drift so the veins very slowly flow.

static float3 procMarble(float2 p, float time, float contrast,
                          int octaves, float3 A, float3 B) {
    float warp = procFbm3(float3(p, time * 0.15), octaves);
    float m = sin((p.x + 4.0 * warp) * 1.0);
    float t = m * 0.5 + 0.5;
    t = procApplyContrast(t, contrast);
    return procPalette(t, A, B);
}

// ============================================================================
// Pattern 10 — Brick wall.
// ============================================================================
//
// Offset rows by half a brick on alternate Y indices. Mortar derived
// from u/v fractional position. Per-brick colour jittered by hash on
// (row, column) so the wall doesn't look stamped from a single tile.

static float3 procBrick(float2 p, float time, float contrast,
                         float3 A, float3 B) {
    // Bricks twice as wide as tall — canonical 2:1 stretcher bond.
    float2 q = float2(p.x * 0.5, p.y);
    float row    = floor(q.y);
    float offset = fmod(row, 2.0) * 0.5;
    float u      = fract(q.x + offset);
    float v      = fract(q.y);

    // Mortar lines: small bands around 0 and 1 in each axis.
    float mw = 0.04 + 0.06 * (2.0 - contrast);
    float mortarU = smoothstep(0.0, mw, u) * (1.0 - smoothstep(1.0 - mw, 1.0, u));
    float mortarV = smoothstep(0.0, mw, v) * (1.0 - smoothstep(1.0 - mw, 1.0, v));
    float brickMask = mortarU * mortarV;        // 1 inside brick, 0 in mortar

    // Per-brick colour jitter + slow flicker (animSpeed.z hooked via time).
    float col = floor(q.x + offset);
    float id  = procHash21(float2(col, row));
    id = fract(id + sin(time * 0.4 + id * 6.2831) * 0.05);
    float3 brick  = procPalette(id, A, B);
    float3 mortar = mix(A, B, 0.5) * 0.35;       // dark muted mortar
    return mix(mortar, brick, brickMask);
}

// ============================================================================
// Dispatcher
// ============================================================================
//
// One uniform block (`u.proc`) carries pattern + palette + shape + anim.
// Layout (matches ProceduralMaterialUniforms on the C++ side):
//
//   proc.colorA  = (Argb.rgb, opacityA)
//   proc.colorB  = (Brgb.rgb, opacityB)
//   proc.shape   = (scale,    contrast, mix, patternIndex)
//   proc.anim    = (animX,    animY,    animZ, octaves)
//
// `mix == 0` is a fast path: caller skips this whole evaluation. Caller
// is responsible for that check — we don't gate inside here so the
// dispatcher stays a flat switch.

struct ProcParams {
    float4 colorA;
    float4 colorB;
    float4 shape;
    float4 anim;
};

static float3 procEvaluate(float2 uv, float elapsedSeconds, ProcParams p) {
    float  scale     = p.shape.x;
    float  contrast  = p.shape.y;
    int    pattern   = int(p.shape.w + 0.5);
    int    octaves   = int(clamp(p.anim.w, 1.0, 8.0));
    float  timeScale = p.anim.z;
    float2 anim      = p.anim.xy;
    float3 A = p.colorA.rgb;
    float3 B = p.colorB.rgb;
    float  t = elapsedSeconds * timeScale;

    // Pre-scaled, pre-translated sample point.
    float2 sp = uv * scale + anim * elapsedSeconds;

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
