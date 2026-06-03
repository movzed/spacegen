# LightClonerLayer

Generator layer that **clones one light configuration into N virtual fixtures**,
arranged via a distribution pattern (ring / helix / grid / Fibonacci-sphere /
random), with per-clone phase offsets, color schemes (uniform / hue chase /
gradient / triadic / complementary), and per-clone Lissajous swarm motion.

This is the SpaceGen equivalent of Notch's *Cloner* node applied to light
emitters: one set of "master" controls drives many fixtures, so the operator
configures the look once and the cluster duplicates itself across the rig.

## Why this layer exists

`BeamLayer` already supports multi-fixture rigs (Linear / Arc layouts up to 8
fixtures). That covers FOH truss arrangements. `LightClonerLayer` exists for
the cases where:

- You want **64 lights in a Fibonacci sphere** orbiting the scene centroid
  ("planetarium" / "fireflies" look).
- You want a **helix of 32 spots** climbing a column, each one a few degrees
  ahead of the last (DNA strand / drill).
- You want a **grid of 24 spots** on the ceiling plane (drop ceiling / Berghain
  light box).
- You want the cluster to **swarm**: each clone follows its own Lissajous orbit
  on top of the shared aim, so the constellation breathes.

`BeamLayer` rigs cap at 8 fixtures and only do Linear/Arc. `LightClonerLayer`
caps at 64 clones and supports 5 distribution patterns + 4 color schemes.

## Architecture: EMITS-MULTIPLE (the Notch way)

`LightClonerLayer` is a `LayerKind::Generator` like `BeamLayer`. Its
`render()` is a no-op — `StructureLayer` is responsible for walking the bus
and pulling light-emitting layers out for the structure PBR pass.

The key extension to `StructureLayer.cpp` is: when it encounters a
`LightClonerLayer`, it calls `cloner->expandSpots(ctx, sink)` which **appends
N `GpuSpot`-equivalent virtual fixtures** to the light list the renderer is
about to upload. From the renderer's perspective, the resulting frame just
has more spots in the array — it has no idea any of them were cloned.

This is preferable to a "wrapper that owns N child `BeamLayer` instances"
because:

- **No layer-rack pollution**: 64 child layers in the rack UI would be a
  disaster. The cloner is one row with one inspector.
- **Cheaper**: We avoid 64 redundant copies of `LFO`/`MotionLFO` objects.
  The cloner stores ONE template config + ONE distribution descriptor.
- **Renderer is agnostic**: `MetalRenderer` knows nothing about cloning. It
  just receives a packed `SpotLight` array. We only raise `kMaxSpots`.
- **Generic across light types**: the same cloner can later emit virtual
  `AreaLight` entries when `AreaLightLayer` lands. The `LightKind` enum
  selects which target array we expand into.

## Distribution patterns

All patterns compute clone positions in a **local frame** centered on
`origin` (which defaults to the scene centroid, or follows the camera). The
local frame is built from `axis` (the pattern's "up" / spine) plus an
arbitrary perpendicular reference.

### Ring

Equal-angle distribution on a circle in the plane perpendicular to `axis`.

```
For clone i in [0..N):
  theta_i = 2*PI * (i / N)
  pos_i   = origin + radius * (cos(theta_i) * right + sin(theta_i) * up)
```

`right` and `up` are derived from `axis` via Gram-Schmidt. Default `axis` is
world +Z (ring lies in XY plane).

### Helix

Ring + linear advance along `axis`:

```
For clone i in [0..N):
  theta_i = 2*PI * turns * (i / (N-1))
  height  = stepHeight * (i - (N-1)/2)
  pos_i   = origin + radius * (cos(theta_i) * right + sin(theta_i) * up)
                  + axis * height
```

`turns` controls how many full revolutions across the N clones.

### Grid

N is interpreted as the cell count; the layer derives `cols x rows` from
`gridCols`. The grid lies in the plane perpendicular to `axis`:

```
For clone i in [0..N):
  col = i % gridCols
  row = i / gridCols
  pos_i = origin + (col - (cols-1)/2) * spacing * right
                + (row - (rows-1)/2) * spacing * up
```

### Fibonacci spiral on a sphere

This is the "uniformly distributed points on a sphere" trick that has been
the gold standard since Saff & Kuijlaars (1997). It avoids the polar
clumping of naive lat/long sampling. We use the **golden-angle increment**:

```
PHI         = (1 + sqrt(5)) / 2          # golden ratio ≈ 1.6180339887
GOLDEN_ANG  = 2*PI / (PHI * PHI)         # ≈ 137.5077640° in radians
            = 2*PI * (1 - 1/PHI)         # equivalent form

For clone i in [0..N):
  # z varies linearly from +1 to -1 across the N samples
  z       = 1.0 - 2.0 * (i + 0.5) / N

  # radius of the latitude circle at this z
  r       = sqrt(max(0.0, 1.0 - z*z))

  # spiral azimuth: each step advances by the golden angle
  theta_i = GOLDEN_ANG * i

  # unit-sphere point
  unit    = vec3(r * cos(theta_i), r * sin(theta_i), z)

  # map into the cloner's local frame and scale by radius
  pos_i   = origin + sphereRadius * (unit.x * right + unit.y * up + unit.z * axis)
```

The factor `2*PI / PHI^2` (equivalently `137.5077...°` per step, the
"golden angle") is the irrational rotation that gives the most even
spreading on any spiral — it's the same principle as sunflower seed
packing. Reference: González (2010), *Measurement of areas on a sphere
using Fibonacci and latitude–longitude lattices*, Mathematical Geosciences.

The `+0.5` offset on the i index (Marques et al. 2013) shifts the samples
half a step away from the poles for better polar coverage on small N.

### Random (deterministic)

Pseudo-random points inside a sphere of radius `randomRadius`, seeded by
`seed`. The point sequence is reproducible for fixed `seed` and `N`:

```
hash3(seed, i):  # 3 floats in [-1, +1], deterministic
   h.x = fract(sin(i*12.9898 + seed*78.233) * 43758.5453) * 2 - 1
   h.y = fract(sin(i*39.346  + seed*11.135) * 24634.6345) * 2 - 1
   h.z = fract(sin(i*73.156  + seed*45.213) * 53758.5453) * 2 - 1

For clone i:
  p     = hash3(seed, i)
  pos_i = origin + randomRadius * p
```

For most operator use cases this looks "random enough"; a true uniform
in-sphere sampler would reject samples outside the unit ball, but for N≤64
the bias of unrejected box sampling is visually negligible and the
determinism (same seed → same arrangement) matters more.

## Per-clone phase

Each clone gets a phase offset `phaseSpread * (i / N)` that propagates into
**every animated parameter** of the template light: its `panLFO`, `tiltLFO`,
`intensityLFO`, `motionLFO`, and the swarm-motion Lissajous.

This is exactly the same mechanism `BeamLayer` uses for its multi-fixture
rigs (see `BeamLayer::fixturePhaseShift`) — the cloner just operates on a
larger N.

- `phaseSpread = 0.0` → all clones move in sync (great for unison sweeps).
- `phaseSpread = 1.0` → the phase wraps once across all N clones (chase /
  wave propagation along the index).
- Intermediate values blend continuously.

## Color schemes

Stage-lighting color theory shorthand:

### Uniform

All clones share `colorStart`. Trivial baseline.

### Hue chase

Convert `colorStart` to HSV, then rotate the hue across clones:

```
hsv0 = rgbToHsv(colorStart)
For clone i:
  hue_i  = fract(hsv0.h + (i / N) * hueSpread)
  rgb_i  = hsvToRgb(hue_i, hsv0.s, hsv0.v)
```

`hueSpread = 1.0` walks the full color wheel across the N clones; `0.5`
walks half the wheel (e.g., red → cyan); `0.083` covers one twelfth (a
typical "warm shift" group).

### Gradient

Linear lerp between two endpoints in RGB:

```
For clone i:
  t      = i / (N - 1)
  rgb_i  = mix(colorStart, colorEnd, t)
```

(Operators are warned in the inspector that RGB lerp through complementary
colors goes through grey at t=0.5 — that's intentional, since some looks
want that grey midpoint, but if you want a "rainbow" lerp you should use
hue chase instead.)

### Triadic

Three colors equally spaced 120° apart on the hue wheel. The clones cycle:
clone 0 → hue0, clone 1 → hue0+120°, clone 2 → hue0+240°, clone 3 → hue0,
...

```
hsv0 = rgbToHsv(colorStart)
For clone i:
  hue_i  = fract(hsv0.h + ((i % 3) * (1.0/3.0)))
  rgb_i  = hsvToRgb(hue_i, hsv0.s, hsv0.v)
```

Reference: Itten's color wheel (Johannes Itten, *The Art of Color*, 1961) —
triadic schemes are perceptually balanced because the three hues sum to
neutral.

### Complementary

Two colors 180° apart. Even clones use `colorStart`, odd clones use the
complement (hue+0.5):

```
hsv0 = rgbToHsv(colorStart)
For clone i:
  hue_i  = fract(hsv0.h + ((i % 2) * 0.5))
  rgb_i  = hsvToRgb(hue_i, hsv0.s, hsv0.v)
```

The result is high contrast — typical "Coachella red/cyan" duotone rigs.

## Swarm motion (per-clone Lissajous)

On top of the shared `panLFO`/`tiltLFO`/`motionLFO` from the template, each
clone can be offset by a small Lissajous orbit in 3D, so the position of
each clone drifts independently:

```
omega_x = swarmFreq * 2*PI
omega_y = swarmFreq * 2*PI * swarmRatioY    # default 1.31 (irrational ratio)
omega_z = swarmFreq * 2*PI * swarmRatioZ    # default 0.79

For clone i:
  ph    = phaseSpread * (i / N)
  dx    = swarmAmp * sin(omega_x * t + ph * 2*PI)
  dy    = swarmAmp * sin(omega_y * t + ph * 2*PI + 1.7)
  dz    = swarmAmp * 0.4 * sin(omega_z * t + ph * 2*PI + 3.1)
  pos_i += dx * right + dy * up + dz * axis
```

The irrational ratios (1.31, 0.79) make the Lissajous figure never repeat
exactly, giving an organic "flocking" feel rather than a strict loop. They
were picked empirically — any pair of mutually irrational ratios works.

## ImGui inspector

The inspector exposes:

- **Light kind** combo: `Spot` (default), `Area` (gated behind a "if Area
  layer support compiled in" preprocessor flag).
- **Clones** slider: 1..64.
- **Pattern** combo: Ring / Helix / Grid / FibSphere / Random.
- Pattern-specific params (radius, axis, spacing, turns, seed) shown
  conditionally.
- **Phase spread** slider: 0..1.
- **Color scheme** combo: Uniform / HueChase / Gradient / Triadic /
  Complementary.
- **Start color** + **End color** pickers (end color only enabled for
  Gradient).
- **Hue spread** slider (only for HueChase): 0..1.
- **Swarm** collapsing header:
  - Amplitude, frequency, ratio sliders.
- **Template light** collapsing header:
  - The cloner embeds the template `BeamLayer`-style controls inline:
    intensity, range, inner/outer cone, base pan/tilt, plus its LFOs.

The expanded "rack" of 64 virtual fixtures never appears in the layer
rack — this is exactly the point of the EMITS-MULTIPLE design.

## Performance

`expandSpots()` is O(N) per frame for one cloner — for N=64 it's a handful
of microseconds of plain CPU math. The MSL shader cost is the same as for
any 64 spots: 64 × per-fragment `dot()` + falloff. On Apple Silicon at
1080p that's well under a millisecond.

The renderer's hard ceiling is `kMaxSpots = 64` after this change (see
`INTEGRATION.md`).

## References

- Saff, E. B., & Kuijlaars, A. B. (1997). *Distributing many points on a
  sphere*. The Mathematical Intelligencer 19.1, 5–11. — The classical
  paper introducing the Fibonacci-spiral solution.
- González, Á. (2010). *Measurement of areas on a sphere using Fibonacci
  and latitude–longitude lattices*. Mathematical Geosciences 42.1.
- Marques, R., et al. (2013). *Spherical Fibonacci point sets for
  illumination integrals*. Computer Graphics Forum 32.4.
- Itten, J. (1961). *The Art of Color*. Reinhold. — Source for the
  triadic / complementary harmonies used in stage-lighting color theory.
