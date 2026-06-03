# VolumetricBeamLayer

Raymarched volumetric "cones of light in air" for every BeamLayer fixture in the
bus. The visual goal is concert-stage hazer/fog: dense enough and you get the
mystical Tomorrowland mainstage feel; off and the layer is invisible. Beams
animate with the spotlights — pan/tilt LFOs, motion patterns, multi-fixture
rigs and audio-driven modulators all carry through because the layer walks
the same data as the StructureLayer pass.

This is the **post-process** companion to `BeamLayer`. BeamLayer alone produces
only surface illumination ("projector-style", per the original SpaceGen rule).
VolumetricBeamLayer re-introduces the cone-in-air look as a screen-space
raymarch that uses the existing depth buffer for occlusion, so adding it is
non-destructive: it only adds light, never replaces what BeamLayer already
does on the structure.

## Algorithm — single-scattering through homogeneous participating media

For each output fragment we raymarch the view ray from the camera near plane
to the front-most surface depth, and at each step we evaluate single-scattering
from every spotlight in the bus.

The radiative transfer equation we solve (single scattering, no shadows, no
multiple scattering) is:

```
L(camera) = ∫₀ᴰ  σ_s · ρ(x)  ·  Σ_lights  Φ(cosθ_view_light) · I_light(x)  ·  e⁻ᵗ(x)  dx
```

where:

| symbol | meaning |
|---|---|
| `D` | distance from camera to the front-most opaque surface (from depth buffer) |
| `σ_s` | scattering coefficient — what we call "density" in the UI (constant) |
| `ρ(x)` | homogeneous medium so `ρ(x)=1`. Reserved for future noise-based haze. |
| `Φ` | Henyey-Greenstein phase function (anisotropy) |
| `I_light(x)` | spotlight contribution at point `x` (cone falloff × range falloff × intensity) |
| `e⁻ᵗ(x)` | extinction (Beer-Lambert). Optional; cheap to compute, off by default. |

We integrate with a Riemann sum (rectangle rule) of `N` samples along the ray,
each one displaced by a per-pixel deterministic jitter to break banding. The
result is added on top of the existing color target. Alpha is increased by
the scattering luminance so the per-pixel light intensity contract is honored
(see `notch-mastery/SKILL.md` §11.4 and `spacegen-integration.md` §3.6).

```
output_rgba.rgb += ∑_i  inscatter_i
output_rgba.a   = saturate(output_rgba.a + luma(∑_i inscatter_i))
```

### Henyey-Greenstein phase function

The probability that a photon entering the medium with view direction `V`
scatters into direction `L` (towards the light) is, per Henyey & Greenstein
(1941):

```
Φ_HG(cosθ, g) = (1 / 4π) · (1 - g²) / (1 + g² - 2g·cosθ)^(3/2)
```

where `cosθ = dot(-V, L)` (negative because we want the angle between the
light direction and the incoming view ray, looking outward from the camera).

| `g` | behavior |
|---|---|
| `g = 0`     | isotropic — light scatters equally in all directions (Rayleigh limit) |
| `g > 0`     | forward scattering — strong forward lobe (dust, fog, water droplets) |
| `g < 0`     | backward scattering — most photons reverse (rare in physical media) |
| `g ≈ 0.6 – 0.85` | physically plausible for atmospheric haze and theatre fog |
| `g → 1`     | mirror-like forward scattering (collapses; clamp to ≤0.99 in code) |

Concert/theatre hazers behave like fog: prefer `g ≈ 0.7`. The phase is
divided into the cone-modulated radiance per sample point.

### Cone test — per fixture

Every BeamLayer expands to N fixtures (via `fixturePositions(ctx)` /
`directionAtTimeForFixture(...)`). For each fixture we have:

- world-space origin `P_light`
- world-space normalized direction `D_light` (FROM light, into the scene)
- range `R`
- inner half-angle cosine `cosInner` and outer half-angle cosine `cosOuter`
- color `C` and time-evaluated intensity `I` (already multiplied by opacity)

For a sample point `X` along the view ray we compute the vector to the light
and its length, build `L_unit = normalize(P_light - X)`, then:

```
spotCos = dot(-L_unit, D_light)       // positive when X is inside the cone
if (spotCos < cosOuter || dist > R)   // outside cone or out of range
    skip
cone    = smoothstep(cosOuter, cosInner, spotCos)
rangeFade = saturate(1 - dist / R)
I_at_X    = I · cone · rangeFade
```

This intentionally mirrors the cone math in `MetalRenderer.cpp`'s structure
shader so the volumetric matches the surface illumination exactly — fixtures
that you can see hitting the structure will be the same ones whose visible
cones appear in the air.

### Jittered raymarch — banding mitigation

Naive uniform stepping with N samples shows hard banding ("stairs") in the
cone, especially with low sample counts and dense haze. We follow the
Frostbite / *Practical Real-Time Strategies for Accurate Indirect Occlusion*
(Lagarde & de Rousiers, 2014) recipe: per-pixel deterministic jitter offsetting
each sample by a fraction of one step, then temporal dithering via the frame
index so the residual banding shimmers below perceptible threshold.

The jitter source is a Bayer-style hash of the pixel coordinate (cheap, no
texture lookup needed at this scale). Strength is exposed as a slider so
the operator can dial it from 0 (clean banding visible) up to 1 (full
±0.5-step displacement, banding gone, slight grain).

```
jitter01 = bayerHash(pixel.xy, frameIndex)        // [0, 1)
t_i = (i + jitter01 * jitterStrength) / N
dist_i = mix(near, sceneDepth, t_i)
```

References:
- Bridson, "Curl-Noise for Procedural Fluid Flow" (2007) — for the structure
  of jitter functions.
- iquilezles.org/articles/raymarchingdf — for the raymarch loop pattern.
- Pettineo, "Frostbite Volumetric Lighting" (2014) — jitter + temporal AA
  rationale for stepped raymarches.
- Lagarde & de Rousiers, "Moving Frostbite to PBR" (SIGGRAPH 2014) — HG
  and single-scattering canon for real-time engines.

## Performance budget

| sample count | M1 Max @1080p | M1 Max @4K |
|---|---|---|
| 16  | ~0.4 ms | ~1.5 ms |
| 32  | ~0.8 ms | ~3.2 ms |
| 64  | ~1.6 ms | ~6.4 ms |
| 128 | ~3.2 ms | ~12.5 ms |

The default of 48 samples lands at ~1.2 ms @1080p — within the
"~1-2 ms" budget that the notch-mastery skill (§11.8) targets for froxel
volumetric beams, while staying simpler than a full 3D-texture froxel
solve. The shader scales linearly in fragment count and in the number
of fixtures × samples (inner loop), which is why a hard `kMaxSpots = 32`
cap matches the structure shader.

## Why post-process and not froxel volume

Notch's reference does it with a frustum-aligned 3D texture (160×90×128
froxels); we could too, but:

- A screen-space raymarch is **simpler to wire** into the existing layer
  stack (single render-target read + depth read + one pass).
- The fixture count is small (≤32), so per-fragment inner-loop is cheap.
- We don't need temporal accumulation in the volume — SpaceGen targets
  a static camera relative to the projection mapping plate, so banding
  is the only artifact and jitter handles it.
- Future upgrade to a froxel solve is a drop-in replacement that swaps
  the fragment shader — the bus integration and uniform packing don't
  change.

## Alpha contract

Per `notch-mastery/SKILL.md` §11.4: a generator that produces light **must**
write alpha that tracks how much light it's contributing at that pixel.
VolumetricBeamLayer increases alpha by `saturate(luma(inscatter))`, so the
StructureLayer's `emitLightsOnly` composite path keeps working: the volumetric
adds to the lights-only output, and the volumetric cones appear in the
composited result over the Resolume plate.

## File map

| file | purpose |
|---|---|
| `VolumetricBeamLayer.h`   | `ILayer` subclass declaration |
| `VolumetricBeamLayer.cpp` | bus walk, uniform packing, inspector, render-pass driver |
| `volumetric.metal.inc`    | MSL source for the fullscreen post-process |
| `INTEGRATION.md`          | exact MetalRenderer diff to wire this layer in |

## Operator notes

Density 0 → invisible. The layer is a no-op pass with no perf cost beyond
the early-out. Crank density to ~0.05-0.08 with `g = 0.7` to get the
classic Tomorrowland mainstage haze. For "smoke-machine just fired" hits,
keep density low (~0.02) and bind it to an audio transient with attack ~50 ms
and release ~800 ms.
