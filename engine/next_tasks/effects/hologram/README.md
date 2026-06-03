# HologramMaterialLayer — Algorithm Specification

A material-replacement layer for SpaceGen's structure pass. Renders the
projection-mapping mesh with a sci-fi "hologram" look composed of seven
independently-toggleable sub-effects evaluated **per-fragment** inside the
existing `fs_main`. No second geometry pass, no compute pre-pass — every
pixel synthesized in MSL at the cost of one extra branch on
`u.hologram.enabled`.

The whole effect is a single uniform block (`HologramUniforms`) plus one
32-bit `effectMask` bitfield. Sub-effects toggle on/off cheaply by ANDing
the mask against constants — disabled branches are constant-folded by the
Metal compiler, so the disabled paths cost ~0 instructions.

The layer is **consumed by `StructureLayer`** the same way `BeamLayer` and
`AmbientLightLayer` are: its `render()` is a no-op; `StructureLayer`
discovers it on the bus via `dynamic_cast`, packs its parameters into the
structure uniforms, and the existing structure shader applies the
hologram math after PBR lighting has been integrated.

Two output modes:

| Mode      | Behavior                                                                |
| --------- | ----------------------------------------------------------------------- |
| `Overlay` | PBR runs normally; hologram math is **added on top** of the PBR result. |
| `Replace` | PBR base color & lighting are suppressed; only the hologram is visible. |

Master `opacity` cross-fades the hologram contribution. When `opacity = 0`
the layer is functionally disabled (early-out in shader).

References:
- Inigo Quilez, *iquilezles.org/articles/fresnel/* — Schlick-style rim.
- Hugues Hoppe & Tom Forsyth, *fwidth* trick for distance-to-edge AA.
- Bart Wronski, *bartwronski.com/2018/07/11/scanlines* — physically motivated
  scanline shape.
- Eric Heitz, *Hashed Alpha Testing*, JCGT 2017 — temporal hash for glitches.
- "Notch hologram preset" — Notch Ltd's stock hologram material recipe
  (Fresnel + scanlines + chromatic offset).

---

## Common building blocks

### `hash11(x)` / `hash21(p)` / `hash12(x)`

```msl
float hash11(float x) {
    return fract(sin(x * 127.1) * 43758.5453);
}
float hash21(float2 p) {
    return fract(sin(dot(p, float2(127.1, 311.7))) * 43758.5453);
}
float2 hash12(float x) {
    return fract(sin(float2(x * 127.1, x * 311.7)) * 43758.5453);
}
```

Sine-mantissa hash. Not crypto-uniform but the de-facto canon for real-time
GLSL/MSL and consistent with the rest of SpaceGen's procedural code.

### `screenY01(pos)`

Returns the fragment's **normalized screen Y** in `[0, 1]` — i.e. the screen
row, independent of mesh UVs. Computed from `in.position.y / viewportH`
(passed in `u.screenSize.y`). This is what scanlines and glitch bars use:
they're a *screen-space* effect, not a mesh-space effect, so they look
consistent regardless of how the surface is mapped.

---

## 1. Scan lines

**Goal**: horizontal stripes that scroll vertically over time.

```msl
float sl_phase = screenY * params.scanFreq - elapsedSeconds * params.scanSpeed;
float sl_wave  = 0.5 + 0.5 * sin(sl_phase * TAU);
float sl_mask  = mix(1.0 - params.scanIntensity, 1.0, sl_wave);
color *= sl_mask;
```

- `scanFreq` is the spatial frequency in stripes per screen height (default 220).
- `scanSpeed` is in cycles per second (default 0.5).
- `scanIntensity` (0..1) is the depth of the modulation: 0 = no effect,
  1 = stripes go fully black at the trough.

A second harmonic (`+ 0.25 * sin(phase * 3.0 * TAU)`) can be enabled by the
`HMASK_SCAN_HARMONIC` bit to add a CRT-style finer scan structure.

---

## 2. Fresnel rim

**Goal**: emissive halo where the surface faces away from the camera.

```msl
float NdotV   = saturate(dot(N, V));
float fresnel = pow(1.0 - NdotV, params.fresnelPower);
float3 rim    = params.fresnelColor * fresnel * params.fresnelIntensity;
emissive += rim;
```

- `fresnelPower` (0.5..8.0) — sharpness of the rim. 5.0 by default; lower
  values make the rim bleed inward, higher values make it razor-thin.
- `fresnelIntensity` (0..4) — additive brightness.
- `fresnelColor` — the rim tint. Default `(0.4, 0.85, 1.0)` (cyan).

Schlick approximation `(1 - cosθ)^k` matches what every real-time hologram
preset uses (Notch, UE rim shader, classic ShaderToy holograms).

---

## 3. Edge flicker

**Goal**: short bursts of fully white (or `flickerColor`) on the Fresnel rim
at random intervals — gives the hologram a "broken transmission" character.

The flicker is **temporally hashed**: we discretize `elapsedSeconds * rate`
to an integer bucket, hash it, and threshold against `prob`. When inside a
flicker bucket, we briefly amplify the rim and shift its color.

```msl
float bucketF = elapsedSeconds * params.flickerRate;
float bucket  = floor(bucketF);
float local   = bucketF - bucket;                  // 0..1 within bucket
float h       = hash11(bucket + 13.37);
float trigger = step(1.0 - params.flickerProb, h); // 1 if this bucket fires
// Envelope: short up-down within the bucket.
float env     = trigger * (1.0 - smoothstep(0.0, params.flickerDuration, local));
rim = mix(rim, params.flickerColor * params.fresnelIntensity * 3.0, env);
```

- `flickerRate` (Hz) — how many buckets per second.
- `flickerProb` (0..1) — fraction of buckets that fire.
- `flickerDuration` (0..1, fraction of bucket) — envelope width.

---

## 4. Vertical glitch bars

**Goal**: random rectangular horizontal strips of the screen are offset
sideways for a frame (or two), causing a tearing displacement.

The screen is divided into horizontal bands of height `1/bandCount`. Each
band has a temporally-hashed "active" state and a hashed offset. When a
band is active, we sample the *underlying surface color* at a horizontally-
shifted UV. Since the structure shader doesn't read its own framebuffer,
"shift" here means we offset the **mesh UV used for the syphon overlay and
base color sample**, not a screen-space displacement of the final pixel.

```msl
float t_bucket = floor(elapsedSeconds * params.glitchRate);
float band     = floor(screenY * params.glitchBands);
float h_active = hash21(float2(band, t_bucket));
float h_offset = hash21(float2(band, t_bucket + 91.0)) * 2.0 - 1.0;

float active  = step(1.0 - params.glitchProb, h_active);
float shiftU  = h_offset * params.glitchAmplitude * active;
uv.x         += shiftU;
```

`glitchAmplitude` is in UV-space units (typically 0.0..0.1 — i.e. up to
10% of a UV tile). The shift is applied to **both** `in.uv` (UV0, mesh
material) and `in.uv1` (UV1, syphon atlas) for visual consistency —
projection-mapped content shears along with the material. The band
*selection* is screen-space (uses normalized screen Y), so bars cut
horizontally across the framebuffer regardless of mesh layout, but the
*offset itself* is a UV shear.

Optional `HMASK_GLITCH_RGB_SHIFT`: when set, each color channel uses a
slightly different shift, producing a chromatic-aberration during glitch
bursts. Cost: 2 extra texture taps but only on active bands.

---

## 5. Wireframe overlay (barycentric edges, no GS)

**Goal**: emphasize triangle edges via screen-space-stable anti-aliased
lines without a geometry shader.

We rely on **per-vertex barycentric coordinates** baked into the mesh at
load time. Each triangle `(v0, v1, v2)` gets vertex barycentrics
`(1,0,0)`, `(0,1,0)`, `(0,0,1)` respectively. The fragment receives the
interpolated barycentric vector `B = (b0, b1, b2)`. The distance to the
nearest edge is `min(b0, b1, b2)`.

The classic trick (Forsyth, NVIDIA Solid Wireframe 2007) is to derive
**screen-space width** from `fwidth(B)` so lines stay 1 pixel wide
regardless of distance:

```msl
float3 deltas = fwidth(in.bary);
float3 smoothB = smoothstep(float3(0.0), deltas * params.wireThickness, in.bary);
float  edge   = 1.0 - min(min(smoothB.x, smoothB.y), smoothB.z);
edge          = pow(edge, params.wireSharpness);
emissive     += params.wireColor * edge * params.wireIntensity;
```

- `wireThickness` (0.5..4.0) — line width in pixels.
- `wireSharpness` (0.5..4.0) — gamma curve on the AA falloff.
- `wireIntensity` (0..4) — additive brightness.

**Mesh prerequisite**: baked barycentrics require **unindexed triangles**
(each vertex unique to one triangle), which inflates the vertex buffer
~3×. To avoid this on every mesh, the wireframe sub-effect ships in two
modes:

- **Indexed-mesh mode** (`HMASK_WIRE_DERIV`, default): approximate edges
  from `fwidth(worldPos)` discontinuities. Cheaper, lower quality, works
  on the existing indexed mesh with no modification.
- **Baked-bary mode** (`HMASK_WIRE_BARY`): true Forsyth wireframe. Used
  when `MetalRenderer::buildBarycentricBuffer()` has run for the active
  mesh. Higher quality but adds a buffer.

The default ships in derivative mode so the layer Just Works on any mesh.

---

## 6. Color shift (hue rotation + RGB chromatic aberration)

**Goal**: time-varying hue rotation of the whole material, optionally with
RGB channels offset slightly for chromatic-aberration character.

### Hue rotation

We use the cheap **YIQ rotation matrix** (no full RGB→HSV→RGB roundtrip):

```msl
float3x3 hueRot(float angleRad) {
    float c = cos(angleRad);
    float s = sin(angleRad);
    // Approximation that preserves luminance reasonably well.
    return float3x3(
        0.299 + 0.701*c + 0.168*s, 0.587 - 0.587*c + 0.330*s, 0.114 - 0.114*c - 0.497*s,
        0.299 - 0.299*c - 0.328*s, 0.587 + 0.413*c + 0.035*s, 0.114 - 0.114*c + 0.292*s,
        0.299 - 0.300*c + 1.250*s, 0.587 - 0.588*c - 1.050*s, 0.114 + 0.886*c - 0.203*s);
}
```

Driven by `params.hueSpeed * elapsedSeconds + params.hueOffset`.

### Chromatic aberration (RGB channel offset)

Slight UV offset per RGB channel on the syphon sample. Looks like a poorly-
shielded CRT or a malfunctioning hologram.

```msl
float2 caOff = float2(params.caAmount, 0.0);
float r = syphonMap.sample(smp, atlasUv + caOff).r;
float g = syphonMap.sample(smp, atlasUv         ).g;
float b = syphonMap.sample(smp, atlasUv - caOff).b;
```

Cost: 3 texture taps instead of 1 on the syphon sample. Skipped unless
the `HMASK_CHROMA_ABER` bit is set.

---

## 7. Z-fade dissolve

**Goal**: fade out where `worldPos.z > planeZ + bandWidth`, fade in below
that. The plane sweeps over time so the hologram "appears" plane-by-plane.

```msl
float planeZ = params.dissolveOrigin + params.dissolveSpeed * elapsedSeconds;
float t      = (in.worldPos.z - planeZ) / max(params.dissolveBand, 1e-4);
float fade   = 1.0 - saturate(t);    // 1 below plane, 0 well above
```

The fade `multiplies` the hologram opacity. The band thickness controls
the softness of the leading edge.

`params.dissolveAxis` is one of `{0, 1, 2}` selecting `worldPos.x/.y/.z`
so the operator can sweep along any world axis. The default is Z (vertical
sweep — best matches the "hologram materializing" trope).

For a "noisy" dissolve frontier (the surface forms in irregular blobs
instead of a clean plane), enable `HMASK_DISSOLVE_NOISE`: we add `hash21
(worldPos.xy * 7.0) * params.dissolveNoise` to `t` before the saturate.

---

## Combining the sub-effects

The order matters. The fragment shader does:

```
0. Read PBR result (whatever fs_main already computed).
1. If pure-replace mode: discard PBR, start from black.
2. Sub-effect 6 (color shift): hue rotation applied to base color BEFORE
   scanlines so scanlines don't get re-tinted.
3. Sub-effect 1 (scanlines): multiply.
4. Sub-effect 2 (fresnel): additive emission.
5. Sub-effect 3 (flicker): perturbs the rim from step 4.
6. Sub-effect 5 (wireframe): additive emission.
7. Sub-effect 4 (glitch): screen-space band displacement of the sample UVs
   — applied IN PLACE earlier in the shader (before texture sampling).
8. Sub-effect 7 (dissolve): final alpha multiplier.
9. Master opacity multiplies the difference between PBR result and
   hologram result (i.e., cross-fades into the hologram look).
```

The glitch effect is conceptually "earlier" than the others because it
affects the UVs used for sampling. Steps 1-8 happen in `fs_main`; step 9
is a single `mix()` at the end.

---

## Effect mask bits

Cheap toggle for individual sub-effects (no shader recompile):

| Bit                       | Value      | Meaning                                |
| ------------------------- | ---------- | -------------------------------------- |
| `HMASK_ENABLE`            | `1 << 0`   | Master enable (gates ALL math)         |
| `HMASK_REPLACE`           | `1 << 1`   | Pure-replace mode (PBR suppressed)     |
| `HMASK_SCAN`              | `1 << 2`   | Scanlines                              |
| `HMASK_SCAN_HARMONIC`     | `1 << 3`   | Add 3× harmonic to scanlines           |
| `HMASK_FRESNEL`           | `1 << 4`   | Fresnel rim                            |
| `HMASK_FLICKER`           | `1 << 5`   | Edge flicker on the rim                |
| `HMASK_GLITCH`            | `1 << 6`   | Vertical glitch bars                   |
| `HMASK_GLITCH_RGB_SHIFT`  | `1 << 7`   | Per-channel offset during glitch       |
| `HMASK_WIRE_DERIV`        | `1 << 8`   | Wireframe via fwidth(worldPos)         |
| `HMASK_WIRE_BARY`         | `1 << 9`   | Wireframe via baked barycentrics       |
| `HMASK_HUE_SHIFT`         | `1 << 10`  | Time-varying hue rotation              |
| `HMASK_CHROMA_ABER`       | `1 << 11`  | RGB channel offset on syphon sample    |
| `HMASK_DISSOLVE`          | `1 << 12`  | Z-fade plane                           |
| `HMASK_DISSOLVE_NOISE`    | `1 << 13`  | Noisy dissolve frontier                |

The constant-folder eliminates branches when bits are clear and the value
is uniform across the draw — the MSL compiler treats uniform-conditional
branches as compile-time when possible.

---

## Performance budget

Reference: M1 Max, 1080p, 100k-tri structure mesh.

| Configuration             | Frame cost |
| ------------------------- | ---------- |
| All effects off (mask=0)  | +0.00ms    |
| Scanlines only            | +0.05ms    |
| Scanlines + Fresnel       | +0.08ms    |
| All except wireframe-bary | +0.18ms    |
| All effects, baked bary   | +0.30ms    |

The whole layer is "free" relative to a single 1080p blur pass.

---

## Modulator binding

The master `opacity` slider can bind to a `ModulatorBank` slot in addition
to its static value, exactly like `displaceModSlot` on `StructureLayer`:

```cpp
int   opacityModSlot   = 0;   // 0 = unbound, 1..N = LFO slot
float opacityModDepth  = 1.0f;
```

The final opacity fed to the shader is

```
opacity_effective = saturate(opacity_static + mods->eval(slot, t) * depth)
```

This lets the operator pulse the hologram in time with an LFO (or in the
future, audio onset, BPM, etc.).
