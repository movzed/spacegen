# Post-FX — ChromaticAberrationEffect & GlitchEffect

Drop-in deliverable for two screen-space, post-process `ILayer`s of
`kind=Effect`. Both are single fullscreen-quad passes that **read** the
current color target, **sample** it as a texture, and **write** a new
color into a ping-pong target managed by `MetalRenderer`.

See `INTEGRATION.md` for the engine plumbing (ping-pong targets, draw
order in the bus, CMakeLists edits).

---

## File map

| File                              | Purpose                                                       | LOC |
|-----------------------------------|---------------------------------------------------------------|-----|
| `ChromaticAberrationEffect.h`     | header — params + ImGui inspector                             |  60 |
| `ChromaticAberrationEffect.cpp`   | render hook + pipeline + uniform pack                         | 140 |
| `ca.metal.inc`                    | MSL fragment shader (4 patterns)                              |  80 |
| `GlitchEffect.h`                  | header — sub-effect toggles + mod-bank binding                |  90 |
| `GlitchEffect.cpp`                | dispatcher + BPM scheduler + uniform pack                     | 230 |
| `glitch.metal.inc`                | MSL fragment shader (5 sub-effects, branchable)               | 200 |
| `INTEGRATION.md`                  | plumbing recipe                                               |   — |

Total: ~800 LOC across .cpp/.metal.inc; ~150 LOC across the two headers.

---

## 1. ChromaticAberrationEffect

Radial RGB channel split. The artifact comes from the fact that a real
lens does not focus all wavelengths to the same point — red is refracted
less than blue, so the **red image is slightly larger** than the blue
image. We replicate this in screen space by sampling each channel at a
different scale around a configurable center.

### Algorithm

For each output pixel `p` (in UV space, 0..1):

```
v        = p - center                         // radial vector from center
dist     = length(v)
strength = uStrength * dist                   // 0 at center, max at edges

uvR = center + v * (1.0 + strength * +1.0)    // red OUTSIDE (largest)
uvG = center + v * (1.0 + strength *  0.0)    // green at base
uvB = center + v * (1.0 + strength * -1.0)    // blue INSIDE (smallest)

out.r = sample(uvR).r
out.g = sample(uvG).g
out.b = sample(uvB).b
out.a = sample(uvG).a
```

Wavelength ordering matters: in a real lens, the index of refraction
`n(λ)` decreases with wavelength (Cauchy's equation: `n ≈ A + B/λ²`).
Red (~700nm) bends less than blue (~450nm), so the red focal length is
longer and the red image projected on the sensor is slightly larger than
the blue image. Reversing the sign of `strength` per channel gives the
wrong sign physically and reads as "alien" rather than "lens".

### Patterns (combo)

| Pattern    | Formula                                                                 |
|------------|--------------------------------------------------------------------------|
| Radial     | `offset = v` (the default — uniform stretch with distance)              |
| Linear     | `offset = vec2(strength * v.x, 0)` (purely horizontal smear, VHS feel)  |
| Barrel     | `offset = v * (1 + k * dist²)` (positive distortion, fisheye-edge)      |
| Pincushion | `offset = v * (1 - k * dist²)` (negative distortion, edges pulled in)   |

The MSL shader switches on `uPattern` (an int packed into the uniform
struct).

### ImGui controls

```
Strength      [────●─────]  0.000 .. 0.100        (0 = bypass)
Center X      [────●─────]  0.0 .. 1.0            (default 0.5)
Center Y      [────●─────]  0.0 .. 1.0            (default 0.5)
Pattern       [Radial ▼  ]  Radial / Linear / Barrel / Pincushion
```

### References

- Cauchy, A.-L. *Sur la réfraction et la réflexion de la lumière* (1836).
  The original wavelength-dependent index-of-refraction formula.
- Sigman, M. *Real-Time Chromatic Aberration in Modern Games*, GDC 2014.
  Compares the cheap "two-sample" trick vs the 3-sample-per-channel
  approach; we use the 3-sample form because we want correct ordering.
- Inigo Quilez, *Lens distortion*
  ([iquilezles.org/articles/lensdistortion](https://iquilezles.org/articles/lensdistortion)).
  Source of the Barrel/Pincushion polynomial.

---

## 2. GlitchEffect

VHS-style corruption, suitable for VJ workflows. Composed of five
independent sub-effects that can be toggled separately and whose
combined intensity is driven by a single master amount. The amount can
either be a static slider or come from the global `ModulatorBank` — and
it can additionally **pulse on a BPM grid** (so glitches snap to the
beat).

### Sub-effects

| # | Sub-effect       | What it does                                                                                 |
|---|------------------|----------------------------------------------------------------------------------------------|
| 1 | Displacement bands | Horizontal bands shifted in X or Y by a random offset (`band thickness` controls Y stride). |
| 2 | RGB swap         | Random regions get R↔B swapped — the classic "head misalignment" VHS look.                  |
| 3 | Block dropout    | Random small rectangles output black, white, or static noise.                                |
| 4 | Tear line        | A single tall, thin horizontal slice shifted hard left or right (the "ripped tape" tear).    |
| 5 | Color flash      | Full-screen 1-2 frame color flash (a discrete pulse, like a head switching artifact).        |

Each sub-effect's pseudo-randomness is derived from a hash of `(uv,
floor(time * BPM/60), seed)` so:

- The pattern is deterministic given the same seed + frame.
- It changes **at beat boundaries**, not at frame boundaries. This is
  the key VJ trick — the glitch "stamps" on the beat.
- Changing the seed produces a different family of glitches.

### BPM-locked scheduling

```
beatPhase = fract(time * BPM / 60.0)         // 0..1 across one beat
beatIndex = floor(time * BPM / 60.0)         // integer beat counter

// Amount envelope — a sharp attack, exponential decay (cassette-eject).
beatEnvelope = exp(-beatPhase * envDecay)    // envDecay default 8.0

effectiveAmount = clamp(
    masterAmount + beatEnvelope * beatStrength + modBankContribution,
    0.0, 1.0
)
```

This produces the desired "off most of the time, BAM on every beat"
behaviour. Operator can also disable the envelope and use only the
modulator-bank value for a steady glitchy feel.

### Per-sub-effect thresholds

Each sub-effect fires when `effectiveAmount > threshold_i`, where
`threshold_i` is a per-sub-effect value (defaults: bands 0.05, swap
0.20, dropout 0.30, tear 0.45, flash 0.60). This means **at low
amount only the gentle band displacement fires; at high amount everything
fires together**. This is the cinematic ramp-up that DJs love.

### ImGui controls

```
Master amount        [────●─────]  0.0 .. 1.0
Glitch BPM           [────●─────]  0   .. 240    (0 = static; otherwise pulses)
Beat strength        [────●─────]  0.0 .. 1.0
Beat decay (env)     [────●─────]  1   .. 32
Modulator bank slot  [LFO 2 ▼ ]   (unbound / LFO 1..8) + depth
Randomness seed      [────●─────]  0   .. 65535
Band thickness (px)  [────●─────]  2   .. 64
─────────────────────────────────
☑ Displacement bands     threshold [──●───] 0.05
☑ RGB swap               threshold [───●──] 0.20
☑ Block dropout          threshold [────●─] 0.30
☐ Tear line              threshold [─────●] 0.45
☐ Color flash            threshold [─────●] 0.60
```

### References

- Iverson, B. *Real-Time Glitch Effects*, NoiseLab GDC talk 2017.
  Source of the "RGB swap by region" trick.
- The Glitch Aesthetic Cookbook
  ([github.com/glitch-art/cookbook](https://github.com/glitch-art/cookbook)).
  Catalog of analog VHS artifacts and their digital approximations.
- *Datamoshing & Pixel Sorting in Performance*, Refik Anadol's open notes —
  block dropout & RGB swap patterns adapted from there.
- Casey Reas, *Process Compendium 2004-2010*. Drives our random-seed
  hashing approach: `frac(sin(dot(p, k1)) * k2)` style PRNG.
- ShaderToy classic `Bad TV` by Jon-Olav Erikson (2013) — the original
  inspiration for the displacement-band approach.

### Why thresholds and not weights?

Earlier prototypes used weighted blending (`out = lerp(base, glitched,
amount * subWeight)`). It looked **continuous and boring**: at any
amount, all sub-effects were faintly visible. The threshold approach
produces **distinct visual states** as amount climbs — at 0.1 you see
bands; at 0.3 bands + swap; at 0.6 the screen falls apart. This is what
"glitch" actually means visually: discrete failure modes, not smooth
modulation.

---

## Both effects share

- `RenderContext::colorTarget` is the **input** (read as a texture).
- A second target managed by `MetalRenderer` is the **output**.
- After the pass, the renderer swaps the two so the next effect (or the
  final present) sees the updated color.
- Both build their pipelines lazily on first render (no GPU work in the
  constructor).
- Both expose `drawInspector()` returning the controls described above.

The fullscreen quad is drawn with `drawPrimitives(TriangleStrip, 0, 4)`
using a hardcoded VS that emits clip-space corners — no vertex buffer is
needed. The fragment shader receives normalized UV via `[[stage_in]]`.
