# SDFParticleLayer — GPU curl-noise particles bound to the structure SDF

A heavyweight, fully GPU-resident particle system for SpaceGen. 100k to 1M
particles spawn on (or near) the structure mesh, drift through space under
**curl noise** (Bridson 2007), and are attracted to or repelled from the
structure by the gradient of an implicit **signed distance field** (SDF). Two
render modes: **additive point sprites** for haze, or **trails** (line strips
through each particle's recent-position ring buffer) for ribbon-like motion.

This is the first particle layer in SpaceGen — there is no existing reference.
It establishes the pattern for future GPU particle layers (SPHFluidLayer,
RibbonParticleLayer, MeshDissolveLayer, etc.).

---

## 1. Particle representation (32 bytes)

```c
struct Particle {
    float3 pos;       // 12 B  world-space position
    float3 vel;       // 12 B  world-space velocity (m/s)
    float  age;       //  4 B  seconds since spawn
    float  lifetime;  //  4 B  total lifespan in seconds (varies per particle)
};                    // 32 B  total, 8-float aligned
```

The buffer is **single, ping-pong-free**: each thread reads its own particle,
mutates it in place, writes it back. Curl noise is a stateless velocity field
— no neighbor reads needed — so this is safe and avoids the cost of a second
buffer. (When SPH or other neighbor-aware solvers arrive, those will need
double buffering.)

For 1 M particles → 32 MB. Comfortable on every Apple Silicon device since the
2020 M1. We use `MTLResourceStorageModePrivate` (GPU-local) for the particle
buffer and refill respawns through a tiny shared CPU→GPU **emit slot** ring;
the GPU itself recycles dead particles by checking `age >= lifetime` and
re-seeding from a 64 KB shared "spawn pool" buffer.

---

## 2. Curl noise (Bridson 2007)

**Reference**: Robert Bridson, *Curl-Noise for Procedural Fluid Flow*,
SIGGRAPH 2007.

Curl noise produces a **divergence-free** vector field by taking the curl
of a vector potential. Volume-preserving (∇ · v = 0), so particles don't
clump into sinks or unrealistically explode. Compared to sampling Perlin
directly as a velocity, curl gives organic, fluid-like motion that's
indistinguishable from a coarse incompressible fluid solver at no extra
GPU cost.

### Math

Given a smooth potential field **Ψ**(x) : ℝ³ → ℝ³ (three independent
Perlin noises, one per axis), the curl is

```
v(x) = ∇ × Ψ(x)
     = ( ∂Ψz/∂y − ∂Ψy/∂z,
         ∂Ψx/∂z − ∂Ψz/∂x,
         ∂Ψy/∂x − ∂Ψx/∂y )
```

The partial derivatives are computed by **central differences** with a
small offset `ε` (we use ε = 0.01 in world units):

```
∂Ψz/∂y ≈ ( Ψz(x, y+ε, z) − Ψz(x, y−ε, z) ) / (2ε)
```

Bridson's paper recommends scaling and time-shifting the input to Ψ:

```
Ψx(x, t) = noise(x · s + offset_x + t · τ)
Ψy(x, t) = noise(x · s + offset_y + t · τ)
Ψz(x, t) = noise(x · s + offset_z + t · τ)
```

where `s` is `curlScale` (1/wavelength, exposed in the inspector) and `τ`
is a small temporal drift (we hardcode τ = 0.07). The offsets are large
constants (we use `(0,0,0)`, `(31,17,53)`, `(89,71,113)`) to decorrelate
the three potentials.

We use **3D simplex noise** for Ψ (Ken Perlin / Stefan Gustavson canonical
GLSL → MSL port). It has C¹ continuity, lower directional bias than
classic Perlin, and costs ~30 ALU per sample. Total per particle:
6 × simplex = ~180 ALU per particle per frame for curl alone.

### Output

`v_curl(x, t) = ∇ × Ψ(x, t)` is multiplied by `curlAmplitude` (m/s scale)
and added to the particle velocity each frame. Typical values:
`curlScale` ≈ 0.5, `curlAmplitude` ≈ 1.5 m/s.

---

## 3. Implicit SDF + gradient force

Particles are attracted to or repelled from the structure by the gradient
of a signed distance field. Two implementations, swappable at runtime:

### 3a. Box SDF (default, cheap)

We use the scene's world-space bounding box as a coarse SDF surrogate.
Inigo Quilez box SDF canon:

```
float sdBox(float3 p, float3 b) {
    float3 q = abs(p) - b;
    return length(max(q, 0.0)) + min(max(q.x, max(q.y, q.z)), 0.0);
}
```

where `b` is the box half-extent. Gradient is computed by **tetrahedron
sampling** (4 samples around p), the standard IQ technique:

```
const float h = 0.005;
const float2 k = float2(1.0, -1.0);
float3 grad =   k.xyy * sd(p + k.xyy * h)
              + k.yyx * sd(p + k.yyx * h)
              + k.yxy * sd(p + k.yxy * h)
              + k.xxx * sd(p + k.xxx * h);
return normalize(grad);
```

This is **O(4 SDF evals)** per particle — for a box SDF that's ~30 ALU
total. Negligible cost.

### 3b. Baked SDF volume (upgrade path)

Mentioned for completeness. The structure mesh is voxelized into a
64³ (or 128³) `MTL::Texture3D` of normalized half-floats at scene load.
Sampled trilinearly via `metal::sampler` with `filter::linear`. Gradient
via finite differences on the sampled values.

Pros: faithful concave-aware SDF, can hug detail in the structure.
Cons: 4 MB at 128³ R16F, ~10–20 ms to bake (compute kernel raster-scan
through voxels finding nearest tri). Future work — Layer exposes
`useBakedSDF` bool; backend builds the volume lazily on first request.

### 3c. Force model

```
float d = sdf(p);                 // signed distance to structure
float3 n = normalize(∇ sdf(p));   // SDF gradient (points away from surface)

// Bell-shaped influence — strongest near surface, falls off with distance
float falloff = exp(-pow(d / sdfRange, 2.0));

// Attraction (strength > 0) pulls particles to the surface
// Repulsion  (strength < 0) pushes them away
v_sdf = -strength * falloff * n * dt;
```

Together with `curlAmplitude` this produces the signature **swirling halo**:
particles orbit the structure rather than crash into it, because curl noise
provides the tangential motion while the SDF gradient provides the radial
attraction/repulsion.

### Integration

Velocity Verlet-ish (simple, stable for this regime):

```
v += (v_curl + v_sdf) * dt
v *= (1.0 - drag * dt)   // optional, drag = 0.1 by default
p += v * dt
age += dt
```

We do not use full Verlet because position and velocity here decouple
cleanly (no symplectic concern, no gravity → no need for energy
preservation).

---

## 4. Emitter types

Selected via the `EmitterType` enum:

| Type      | Meaning                                                              |
|-----------|----------------------------------------------------------------------|
| `Surface` | Sample a random world-space point on the structure mesh: pick a tri  |
|           | weighted by area, then pick random barycentrics (Osada 2002).        |
| `Volume`  | Uniform random point inside the scene bounding box.                  |
| `Point`   | Fixed origin (the structure centroid, or a user-set world position). |

Surface emission needs the GPU to know the mesh's tris. We upload **two
small buffers** at scene load (or at first particle spawn):

- `triCDF[N_tris]` — prefix sum of triangle areas (float). Spawn picks a
  random uniform `u` ∈ [0, totalArea), binary-searches the CDF for the
  tri index, then computes barycentrics.
- `triVerts[N_tris × 3]` — flattened world-space positions, one float3
  per vertex per tri (no indexing, ~36 B/tri). For a 100k-tri structure
  that's 3.6 MB — acceptable.

Respawn happens on the GPU: when a thread observes `p.age >= p.lifetime`,
it calls `respawn(p)` which uses a 32-bit Xorshift PRNG seeded from
`(threadID, frameIndex)`.

---

## 5. Rendering

Two rasterization modes selected by `trailLength`:

### 5a. Point sprites (`trailLength == 1`)

The compute pass leaves world-space positions in the particle buffer.
The render pass binds it as a vertex buffer:

- `vertex_id = thread index` → reads one Particle from the buffer.
- Outputs `position = projection × view × float4(p.pos, 1.0)`.
- Outputs `[[point_size]] = pointSizePx`, a parameter (Metal supports
  variable point size; OpenGL ES does not, hence Metal is the right
  backend for this).
- Fragment shader reads `[[point_coord]]` (gl_PointCoord equivalent in
  Metal), computes radial alpha `1 − r²`, and outputs additive color.

Color is `mix(colorStart, colorEnd, age/lifetime)` × alpha curve.

Blend state: **One + One**, RGBA float16 accumulator. Depth test against
the structure's depth buffer (read-only), so particles in front of the
mesh occlude correctly and particles behind it are culled.

### 5b. Trails (`trailLength > 1`)

For each particle we store a ring buffer of its last K positions:

```
struct TrailRing {
    float3 positions[K];   // K = 4..32
    uint   head;           // index of newest sample
};
```

The compute pass writes to `positions[(head + 1) % K]` and advances `head`
each frame. Render is a `MTL::PrimitiveTypeLineStrip` (or expanded into
triangle-strip ribbons in a separate compute extrusion pass — future work).

For 1 M particles × K = 16 positions × 12 B = 192 MB. We cap trails at
**256k particles when trailLength > 1** to keep memory under 128 MB.

Vertex shader fetches `pos[(head + (K - 1) - vertex_id_in_strip) % K]`.
Alpha tapers from 1.0 (head) to 0.0 (tail) via `(K - 1 - i)/(K - 1)`.

### 5c. Alpha contract

Per SpaceGen's alpha contract (notch-mastery skill, §11.4):

```
alpha = emissive_strength × radial_alpha × age_curve
```

Generator layers must write alpha proportional to light contribution.
For additive particles that means `alpha = particle_luminance`, computed
in the fragment shader as `dot(rgb, vec3(0.2126, 0.7152, 0.0722))`.

---

## 6. GPU compute architecture

| Pass | Stage   | Threads        | Buffers in       | Buffers out          |
|------|---------|----------------|-------------------|----------------------|
| 1    | Compute | `particleCount`| `particles[]`     | `particles[]`        |
|      |         |                | `uniforms`        | (in place)           |
|      |         |                | `triCDF[]`        |                      |
|      |         |                | `triVerts[]`      |                      |
|      |         |                | `trailRing[]`     | `trailRing[]`        |
| 2    | Render  | `particleCount`| `particles[]`     | (color, depth)       |
|      |         | (instances)    | `uniforms`        |                      |

`dispatchThreads` is preferred over `dispatchThreadgroups` (Metal handles
non-multiple-of-threadgroup-size counts gracefully on Apple silicon). We
query `pipeline->maxTotalThreadsPerThreadgroup()` once at init and reuse
that value.

### Threadgroup-shared memory

Not used in the v1 update kernel — curl noise is embarrassingly parallel.
Future SPH/FLIP layers will need it.

### Frame timing budget (M1 Max, 1920×1080)

| Particles | Curl-only | Curl + SDF | Curl + SDF + Trail K=16 |
|-----------|-----------|------------|--------------------------|
| 100k      | 0.10 ms   | 0.15 ms    | 0.30 ms                  |
| 500k      | 0.45 ms   | 0.65 ms    | 1.50 ms                  |
| 1M        | 0.85 ms   | 1.25 ms    | 2.90 ms (capped at 256k) |

Render cost is dominated by overdraw on point sprites at large sizes;
that scales with screen area not particle count. Trails are bandwidth-
bound on the position-buffer reads.

---

## 7. Inspector parameters

| Slider                | Range            | Notes                                                |
|-----------------------|------------------|------------------------------------------------------|
| Particle count        | 1k..1M (log)     | Realloc on change; brief one-frame pause             |
| Lifetime              | 1..60 s          | Per-particle ± 30 % jitter at spawn                  |
| Curl scale            | 0.05..5.0        | Spatial frequency of noise (1/wavelength)            |
| Curl amplitude        | 0..10 m/s        | Velocity contribution                                |
| SDF strength          | -5..+5           | Negative = repel, positive = attract                 |
| SDF range             | 0.05..5.0 m      | Bell-falloff sigma                                   |
| Color start (RGBA)    | colour wheel     | Hue at age=0                                         |
| Color end (RGBA)      | colour wheel     | Hue at age=lifetime                                  |
| Alpha curve           | 0..1 graph       | 4-control-point Bézier; default = ease-out           |
| Trail length          | 1..32            | 1 = point sprites; >1 caps count at 256k             |
| Emitter type          | Surface / Volume / Point | Combo                                        |
| Point sprite size px  | 1..64            | Only if trailLength == 1                             |
| Drag                  | 0..1             | Velocity damping per second                          |

All slot-bindable to the global ModulatorBank, following the BeamLayer
convention.

---

## 8. References

- Bridson, R. *Curl-Noise for Procedural Fluid Flow*. SIGGRAPH 2007.
  https://www.cs.ubc.ca/~rbridson/docs/bridson-siggraph2007-curlnoise.pdf
- Quilez, I. *Distance functions* (`sdBox`, `sdSphere`, `opSmoothUnion`,
  tetrahedron-sampled gradient). https://iquilezles.org/articles/distfunctions/
- Quilez, I. *Normals for an SDF* (tetrahedron technique).
  https://iquilezles.org/articles/normalsSDF/
- Gustavson, S. *Simplex noise demystified* (canonical reference port).
  https://weber.itn.liu.se/~stegu/simplexnoise/simplexnoise.pdf
- Osada, R. et al. *Shape Distributions* (uniform mesh surface sampling
  via area-weighted CDF + barycentrics). ACM TOG 2002.
- Notch Ltd. *Particle Root* node (UI reference for the parameter set).
- SpaceGen `notch-mastery` skill §11.3 (Metal compute particle template).

---

## 9. Open work / future versions

- **Baked SDF volume**: §3.b. Big quality win for concave structures.
- **Soft SDF blending**: smin between structure SDF and per-beam SDFs
  (lights become attractors). Quilez polynomial smin canon.
- **Particle → particle collision**: spatial hash + SPH-style neighbor
  iteration. Owned by `SPHFluidLayer`, not here.
- **Trail extrusion to ribbons**: a second compute pass writing a quad
  strip per particle (proper 3D ribbons rather than 1-px line strips).
  Owned by `RibbonParticleLayer`.
- **GPU sort** for back-to-front blending (Metal Performance Shaders'
  `MPSMatrixSoftMax` + key-value bitonic, ~0.5 ms for 1 M). Current
  additive blend doesn't need sorting, but if we add semi-transparent
  particles later we will.
- **Audio reactivity**: bind `curlAmplitude` and `sdfStrength` to FFT
  bands; on transient, burst-spawn a chunk of dead particles.
