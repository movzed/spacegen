# ProceduralMaterialLayer — Algorithm Specification

A shader-side procedural texture generator for SpaceGen's structure pass.
Replaces (mixes with) the structure's `baseColor` with one of ten patterns
evaluated **per-fragment** in MSL. No bitmap loads, no precomputed tables,
no compute pre-pass — every pixel of every frame is synthesized in the
existing `fs_main`.

The whole effect is a single uniform `procTextureType` (int) plus a small
parameter block. The shader dispatches to one of ten functions and blends
the result into `baseTex` **before** the existing Syphon mix runs, so the
operator can still composite a live feed on top of a procedurally-shaded
structure.

References:
- Inigo Quilez, *iquilezles.org/articles* — canonical implementations of
  Voronoi, fbm, smoothmin, palette, hex grid, marble.
- Robert Bridson, *Curl-Noise for Procedural Fluid Flow*, SIGGRAPH 2007.
- Steven Worley, *A Cellular Texture Basis Function*, SIGGRAPH 1996.
- Ken Perlin, *Improving Noise*, SIGGRAPH 2002 — used for simplex.

---

## Common building blocks

All patterns share three primitives:

### 1. `hash21`, `hash22`, `hash33` — IQ's `iqint` style

```msl
float  hash21(float2 p);   // p ∈ ℝ² → [0, 1)
float2 hash22(float2 p);   // p ∈ ℝ² → [0, 1)²
float3 hash33(float3 p);   // p ∈ ℝ³ → [0, 1)³
```

Implemented via the standard sine-mantissa trick:

```
hash21(p) = fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453)
```

This is **not** cryptographically uniform but is the de-facto canon for
real-time procedural texture work. Faster than integer-hash variants and
matches every IQ Shadertoy reference.

### 2. `valueNoise(uv)`

Bilinear value noise. Cheaper than simplex, perceptually fine when
laundered through fbm.

```
n = mix(mix(h00, h10, smoothstep(0,1,f.x)),
        mix(h01, h11, smoothstep(0,1,f.x)),
        smoothstep(0,1,f.y))
```

### 3. `fbm(uv, octaves)`

Standard 5-octave fractal Brownian motion. Octave count is clamped
[1, 8] to bound shader cost. Lacunarity = 2.0, gain = 0.5 (IQ canon).

```
amp = 0.5
for i in [0, octaves):
    v += amp * valueNoise(uv * freq + time)
    freq *= 2.0
    amp  *= 0.5
```

### 4. `palette(t, A, B, opA, opB)`

Two-color cosine-blend palette. `opA` and `opB` are the per-color
opacity sliders from the inspector — they let the operator desaturate
toward neutral grey when the source structure already has color.

```
col = mix(A, B, smoothstep(0, 1, t))
```

---

## Pattern 1 — Voronoi cells (Worley noise)

**Math.** Tile the plane with a grid of cells. In each cell, hash one
feature point. For a query point `p`, return the squared distance to the
**nearest** feature point across the 9-neighbourhood (F1 norm).

Reference: Worley 1996; the 3×3 neighborhood scan is the IQ tutorial
*"voronoi distances"* (`iquilezles.org/articles/voronoilines`).

```msl
float voronoiF1(float2 p) {
    float2 i = floor(p);
    float2 f = fract(p);
    float minD = 1e9;
    for (int y = -1; y <= 1; y++)
    for (int x = -1; x <= 1; x++) {
        float2 g = float2(x, y);
        float2 o = hash22(i + g);        // feature offset in cell
        o = 0.5 + 0.5 * sin(time + 6.2831 * o);   // animated jitter
        float2 r = g + o - f;
        minD = min(minD, dot(r, r));
    }
    return sqrt(minD);
}
```

**Output modes (selected by `contrast`):**
- `contrast < 0.5` → smooth filled cells (`1 - F1`)
- `contrast ≥ 0.5` → crack lines (`smoothstep(edge, edge+aa, F1)`)
  where `edge` derives from contrast.

Animation: time-modulated feature points (the `0.5 + 0.5 * sin(...)`
trick is straight from IQ's `2d distance functions to a polygon` page).

---

## Pattern 2 — Simplex / Perlin fbm

Cheaper Perlin variant: bilinear value noise + fbm.

Reference: Perlin 2002; IQ's *fbm* article
(`iquilezles.org/articles/fbm`).

```msl
float t = fbm(p * scale + animOffset, octaves);
return palette(t, A, B);
```

Octave count is on the inspector (1..8). Contrast remaps `t` via:

```
t = saturate((t - 0.5) * contrast + 0.5);
```

---

## Pattern 3 — Curl noise

Bridson 2007. The curl of a 2D potential field gives a divergence-free
velocity field (looks like fluid swirls). We bake the **streamlines**
into colour by feeding integrated path length to the palette.

Reference: Bridson, *Curl-Noise for Procedural Fluid Flow*, SIGGRAPH 2007;
`iquilezles.org/articles/warp` for the related domain-warp trick.

```
ψ(p, q) = fbm(p, q, octaves)
curl(p, q) = ( ∂ψ/∂q, -∂ψ/∂p )       (analytic 2D curl)
```

We approximate the partials with central differences:

```msl
float eps = 1e-2;
float dpsi_dx = (fbm(p + eps_x) - fbm(p - eps_x)) / (2*eps);
float dpsi_dy = (fbm(p + eps_y) - fbm(p - eps_y)) / (2*eps);
float2 v = float2(dpsi_dy, -dpsi_dx);
```

Visualised colour: `length(v) * 4` fed to palette. Time animates the
underlying `ψ` (3D fbm sampled at `(p.x, p.y, t)`).

---

## Pattern 4 — Hexagonal grid

The "skewed-square hex coord" trick:

Reference: IQ's *hexagonal tiling*
(`iquilezles.org/articles/hexagons`).

```
hex(p):
    p.x *= 0.5773 * 2          // 2/sqrt(3)
    p.y += 0.5 * mod(floor(p.x), 2)
    p = abs(fract(p) - 0.5)
    return abs(max(p.x * 1.5 + p.y, p.y * 2.0) - 1.0)   // edge distance
```

Edge AA: `smoothstep(thresh, thresh + fwidth(d), d)` so the cells
anti-alias under any zoom level.

---

## Pattern 5 — Multi-scale checker

Layered checker — sum of `sign(sin(x*k) * sin(y*k))` at multiple
frequencies, normalized to [0, 1]. Gives a "dirty" checker that doesn't
moiré at distance because higher-frequency layers wash out under
mip/derivative-driven AA.

```msl
float c = 0;
float amp = 1;
for (int i = 0; i < octaves; i++) {
    c += amp * sign(sin(p.x) * sin(p.y));
    p *= 2.0;
    amp *= 0.5;
}
return saturate(c * 0.5 + 0.5);
```

AA via `fwidth(p)` clamp on the innermost frequency.

---

## Pattern 6 — Concentric rings / Saturn rings

```
d = length(p - center)
t = fract(d * scale - time * animSpeed)
band = smoothstep(0.5 - 0.5*contrast, 0.5 + 0.5*contrast, t)
```

Optional 2-color palette with `mix(A, B, band)`. Setting `animSpeed`
positive gives outward expansion (sonar pings); negative gives inward
collapse.

Saturn-style: modulate `band` by an `fbm` rotated 90° to break radial
symmetry — `t += 0.3 * fbm(rotate90(p))`.

---

## Pattern 7 — Voronoi shatter (cells with gaps)

Same F1 scan as pattern 1, but we also compute F2 (the **second**
nearest feature point) and shade:

```
edge = F2 - F1            // distance to nearest cell boundary
gap  = smoothstep(0, contrast*0.1, edge)
cellId = hash21(featurePoint_id_of_F1)
```

`gap = 0` along cell boundaries, `gap = 1` deep inside a cell.
Colour = `mix(B, palette(cellId, A, ...), gap)` — gives shattered
plates with mortar between them. Used in IQ's *voronoi shatter* demo
and Notch's "broken glass" block.

---

## Pattern 8 — Wood grain

Lévy/IQ recipe: take elliptical rings around the wood axis, perturb
the radius with fbm, threshold to grain bands.

```
q = rotate(p, woodAngle)
r = sqrt(q.x*q.x*0.1 + q.y*q.y)        // elongated isolines
r += 0.5 * fbm(q * 3.0, octaves)       // irregular grain
g = fract(r * scale)
g = smoothstep(0.4 - 0.4*contrast,
                0.4 + 0.4*contrast, g)
return palette(g, darkWood, lightWood)
```

The elliptical `sqrt(0.1*x² + y²)` stretches the rings along the
"plank" direction — the canonical wood-grain trick from IQ's *wood*
shader notes.

---

## Pattern 9 — Marble

Perlin's original 1985 example. Sinusoid modulated by noise:

```
m = sin( (p.x + 4 * fbm(p, octaves)) * scale )
m = pow(saturate(m * 0.5 + 0.5), contrast * 4)
return palette(m, veinColor, baseColor)
```

The factor of 4 on `fbm` controls how much the veins warp; higher = more
chaotic. Time can add to `p.x` for slowly-drifting marble (looks like
liquid metal once you slow it down enough).

---

## Pattern 10 — Brick wall

Offset rows: every other row is shifted by half a brick.

```
row    = floor(p.y * scale)
offset = mod(row, 2) * 0.5
u      = fract(p.x * scale + offset)
v      = fract(p.y * scale)
mortar = step(u, mortar_w) + step(1 - mortar_w, u)
       + step(v, mortar_w) + step(1 - mortar_w, v)
mortar = saturate(mortar)
brickId = hash21(float2(floor(p.x * scale + offset), row))
brickCol = palette(brickId, A, B)        // per-brick colour jitter
return mix(brickCol, mortarColor, mortar)
```

Animation: feed `time * animSpeed` into the hash so individual bricks
flicker between the two palette colours (good for techno breakdown
moments — "the wall is alive").

---

## Animation model

Every pattern reads two uniforms:

```
animSpeed.xy   — per-axis offset speed (uv units / second)
animSpeed.z    — z-time (used by 3D fbm / curl)
```

The shader computes `animOffset = animSpeed.xy * time` and adds it to
the sample point before pattern evaluation. Patterns that already use
time internally (Voronoi feature jitter, brick flicker) read
`time = elapsedSeconds * animSpeed.z`. This means **all** animation is
driven by one CPU-pushed `elapsedSeconds` value — no per-pattern
plumbing.

---

## Why this design

- **One shader, ten patterns**: a single `kStructurePbrMSL` compile.
  Avoids the variant-explosion that plagues game engines.
- **Per-fragment, no scratch memory**: works at any resolution, any
  mesh density. No tile RT, no compute pre-pass.
- **Lit on top**: the procedural colour becomes `baseColor`, then the
  existing GGX PBR lighting integrates it normally. The operator
  doesn't lose roughness/metallic — those still work.
- **Mix with Syphon**: insertion point is **before** the Syphon mix so
  the operator can dial in `[procedural ← → Syphon overlay]` smoothly.
- **Mix slider**: `0 = pure baseColor (no effect)`, `1 = pure pattern`.
  The mix is on the inspector so the operator can fade procedurals in
  and out without disabling the layer.

---

## Inspector parameters (summary)

| Param        | Type        | Range       | Notes |
|--------------|-------------|-------------|-------|
| pattern      | int (combo) | 0..9        | Which of 10 patterns |
| colorA       | vec3        | sRGB        | Primary palette colour |
| colorAOpacity| float       | 0..1        | Desaturate A toward grey |
| colorB       | vec3        | sRGB        | Secondary palette colour |
| colorBOpacity| float       | 0..1        | Desaturate B toward grey |
| scale        | float (log) | 0.05..50    | Frequency / cell size |
| animSpeedXY  | vec2        | -2..2       | Pattern drift per axis |
| animSpeedZ   | float       | 0..4        | Time multiplier |
| contrast     | float       | 0..2        | Pattern-specific (see each) |
| octaves      | int         | 1..8        | fbm depth (where applicable) |
| mix          | float       | 0..1        | 0 = invisible, 1 = full replace |

`mix == 0` is a free path in the shader: a `step(1e-4, mix)` guard
skips all pattern math and returns the original `baseTex`. Operator
can leave the layer enabled with `mix = 0` for instant fade-in.

---

## File layout

```
procedural_material/
├── README.md                   — this file
├── ProceduralMaterialLayer.h   — class declaration
├── ProceduralMaterialLayer.cpp — ImGui inspector + render() wiring
├── patterns.metal.inc          — MSL: 10 pattern fns + dispatcher
└── INTEGRATION.md              — exact insertion points in MetalRenderer
```
