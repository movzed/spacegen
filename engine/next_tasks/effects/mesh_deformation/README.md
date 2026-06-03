# MeshDeformationLayer

Rich, chainable, multi-stage vertex deformation pipeline for SpaceGen.
Replaces the simple `displaceAmount` + `twistAmount` knobs in
`StructureLayer` with a Notch-style deformer stack: each operator is a
small inline function in the structure vertex shader, applied in order
to the local-space position (and, for operators that bend the normal
basis, to the normal).

The whole chain is evaluated **in the vertex shader** so it scales to
millions of triangles. The CPU side packs a fixed-size array of
`Op` structs into a uniform block; the shader's dispatcher loops the
active ops and switches on `op.type`.

---

## Why a stack and not 8 booleans?

The existing `displaceAmount` / `twistAmount` are two parallel knobs:
the operator can run both, but not _in a specific order_. A Notch /
Houdini-style deformer is fundamentally **ordered**:

    Spherify (0.3) -> Curl noise (0.6) -> Twist Z -> Bend X -> FFD

gives a totally different result from

    FFD -> Bend X -> Twist Z -> Curl noise (0.6) -> Spherify (0.3)

so the API has to be an ordered, drag-reorderable list. We pick a
fixed cap of `kMaxOps = 16` so the GPU side is a static array.

Each op has:
- `enabled`            — boolean bypass
- `intensity` ∈ [0, 1] — global wet/dry for the op
- per-op parameters    — packed into two `vec4`s on the GPU
- one ModulatorBank slot — drives `intensity` (LFO from the global bank)
- per-op modulator depth

A disabled op is skipped on the GPU (the dispatcher branches on
`type == OpType::None || intensity == 0`).

---

## The 8 operators

### 1. Curl noise displacement
**Math.** Given a smooth scalar noise field `n(p)` (gradient noise),
the **curl** of a 3D vector field built from three orthogonal copies of
`n` is divergence-free:

    F(p) = ( n(p + a), n(p + b), n(p + c) )
    curl(F)(p) = ( ∂F.z/∂y - ∂F.y/∂z,
                   ∂F.x/∂z - ∂F.z/∂x,
                   ∂F.y/∂x - ∂F.x/∂y )

In code we estimate the partial derivatives with finite differences
`ε = 1e-3`:

    float3 dFdx = (F(p + e_x*ε) - F(p - e_x*ε)) / (2ε);

`curl(F)` is a smooth, divergence-free, animated vector field. We
displace the vertex by `curl(F) * amount` to produce flowing,
turbulence-like motion that does NOT puff up the mesh (because the
field is divergence-free — volume-preserving in the limit).

**Time evolution.** `p` is offset by `time * speed * float3(1, 1.3,
0.7)` so the noise drifts through space at slightly different rates per
axis (Bridson's recommendation in "Curl Noise for Procedural Fluid
Flow", SIGGRAPH 2007).

**Params.** `scale` (spatial frequency), `amount` (m), `speed` (Hz).

**Reference.** R. Bridson, J. Hourihan, M. Nordenstam,
*Curl-Noise for Procedural Fluid Flow*, SIGGRAPH 2007.

---

### 2. Voronoi displacement
**Math.** Hash-based 3×3×3 Voronoi (a.k.a. Worley noise). For a query
point `p`, find the cell center `c*` of the nearest neighbor among the
27 cells around `floor(p)`. Each cell `(i, j, k)` has a jittered
center `c_{i,j,k} = (i, j, k) + hash3(i, j, k)`.

Displacement vector:

    d = (c* - p) * shatter

with `shatter ∈ [-1, 1]`. Negative shatter pushes vertices _away_ from
the cell center (expands cells); positive pulls _toward_ (shrinks
cells — creates "shattered" gaps).

**Params.** `scale` (cell size, m), `shatter` ∈ [-1, 1], `jitter`
∈ [0, 1] (how randomly cell centers are placed).

**Reference.** S. Worley, *A Cellular Texture Basis Function*,
SIGGRAPH 1996.

---

### 3. Twist around axis
**Math.** Rotate the vertex around an axis `a` by an angle proportional
to its projection on `a`:

    θ = (p · a) × amount
    p' = R(a, θ) · p

where `R(a, θ)` is the Rodrigues rotation matrix. Generalizes the
existing Z-only twist to any axis (and the inspector exposes a
`vec3 axis` plus a one-shot "axis = X/Y/Z" combo).

**Params.** `axis` (unit vector), `amount` (rad / m), `center` (pivot,
m).

---

### 4. Bend
**Math.** Cinema 4D / 3ds Max-style bend: pick an axis `a` (bend along
which the height varies), pick a bend-direction axis `b` perpendicular
to `a`. Compute the height `h = (p - center) · a`. Then rotate the
"slab" at height `h` by angle `θ(h) = h × angle` around the axis
`c = a × b`. The bend wraps the geometry around an imaginary cylinder
whose radius depends on `angle`:

    R = 1 / |angle|   (when angle ≠ 0)

Empty when `angle = 0` → identity. The implementation uses a Rodrigues
rotation around `c` for every vertex; this is mathematically equivalent
to bending the slab.

**Params.** `axis` (along which to bend), `bendDir` (direction the
geometry bends toward), `angle` (rad/m), `center`.

**Reference.** Barr, *Global and Local Deformations of Solid
Primitives*, SIGGRAPH 1984 (canonical "bend"/"twist"/"taper"/"squash"
trio).

---

### 5. Taper
**Math.** Scale the perpendicular components linearly along the taper
axis:

    h = (p - center) · a
    s = 1 + h × amount  (clamped to ≥ 0)
    p' = center + a*(p · a) + s × (p - a*(p · a))

`amount > 0` widens at one end, `amount < 0` narrows. Identity when
`amount = 0`.

**Params.** `axis`, `amount` (1/m), `center`, `clamp` (whether to clamp
`s` ≥ 0 to prevent inversion).

**Reference.** Barr 1984 (same paper as Bend).

---

### 6. Wave
**Math.** A traveling sinusoid along a direction `dir`, displacing the
vertex along its normal:

    phase = (p · dir) × freq - time × speed
    p' = p + N × sin(phase) × amount

This is the classic ripple-on-water / corrugated-iron effect. Setting
`dir = normal` produces breathing; setting `dir` along a world axis
produces a wave traveling through the mesh.

**Params.** `dir` (unit vector), `freq` (1/m), `speed` (Hz), `amount`
(m), `displaceAlong` (0 = vertex normal, 1 = world up, 2 = `dir`
itself).

---

### 7. Spherify
**Math.** Lerp the vertex toward a sphere of radius `R` centered at
`center`:

    n = normalize(p - center)
    pSphere = center + n × R
    p' = mix(p, pSphere, amount)

`amount = 0` → identity. `amount = 1` → vertices land exactly on a
sphere (the mesh becomes a sphere; topology unchanged). Useful as a
"recall to neutral form" operator at the end of a chain.

The center defaults to the mesh centroid; the inspector lets the
operator override it. Radius can be auto-fit (mean distance from
centroid) or operator-specified.

**Params.** `center`, `radius`, `amount` ∈ [0, 1].

---

### 8. FFD-lite (8-point trilinear)
**Math.** Eight control points at the corners of an axis-aligned
bounding box `B`. For a vertex `p`, compute normalized lattice
coords:

    u = (p - B.min) / (B.max - B.min)   (clamped to [0, 1])
    w_000 = (1-u.x)(1-u.y)(1-u.z)
    w_100 = u.x   (1-u.y)(1-u.z)
    ...
    w_111 = u.x   u.y   u.z
    p' = Σ w_ijk × C_ijk

where `C_ijk` are the operator-dragged corner positions. When all
corners sit at their original bbox positions, `p' = p` (identity).
When the operator drags one corner, the deformation falls off
trilinearly to the others.

This is "FFD-lite" because the canonical Sederberg-Parry FFD uses a
cubic-Bezier 4×4×4 lattice. We use 1st-order trilinear with 8 control
points — fewer DOFs, but enough for crushes, stretches, and "leaning"
warps, and trivially fast in the vertex shader (8 mads per vertex).

**Params.** `bboxMin`, `bboxMax`, `corners[8]` (delta from bbox
corner; identity = all zeros).

**Reference.** T. Sederberg, S. Parry, *Free-Form Deformation of Solid
Geometric Models*, SIGGRAPH 1986.

---

## Order matters

The dispatcher applies operators **in the order they appear in the
list**. The inspector lets the operator drag-reorder rows. Common
recipes:

| Recipe                           | Order                              |
|----------------------------------|-------------------------------------|
| Liquid blob breathing           | Spherify → Curl noise → Wave       |
| Statue with carved cracks       | Voronoi → Bend                     |
| Banner wave + flag flutter      | Wave → Curl noise (small amount)   |
| Cinema 4D classic               | Bend → Twist → Taper               |
| Architectural unfolding         | FFD → Twist → Spherify             |

---

## Normal handling

Each operator only displaces position. The normal is **recomputed
per-vertex** from the displaced position using analytical Jacobians
where cheap (Twist, Bend, Taper, Spherify rotate the normal by the
same Rodrigues rotation as the position; FFD recomputes via the
Jacobian of the trilinear map; Curl/Voronoi/Wave leave the normal
unchanged at v1 — operator should add a low-amount Wave + curl with
matching small amplitude, lighting will read as a slightly noisier
surface, the visual error is acceptable for projection-mapping use).

For high-quality normals at large displacements, a separate compute
shader pass could rebuild normals from neighboring vertices — out of
scope for v1.

---

## Modulator integration

Each op has `intensityModSlot` (0..N, 0 = unbound) and
`intensityModDepth`. On the CPU, before packing the uniform, we
compute the effective intensity:

    intensityEff = clamp(op.intensity + mods->eval(slot, t) * depth, 0, 1)

(intensity is clamped to [0, 1]; depth can be > 1 if the operator
wants the LFO to fully sweep the range).

A future extension lets each op also subscribe a `paramAModSlot` to
drive one chosen per-op parameter (e.g. wave frequency). For v1, only
intensity is modulated — keeps the uniform layout small.

---

## Cost

- Each op: 5-30 ALU + 1-3 noise lookups (curl needs ~6 hash lookups
  for finite-difference gradient).
- 8 ops worst case ≈ ~200 ALU + ~50 hash lookups per vertex.
- On a 100k-tri mesh with shared edges (~50k vertices), that's
  ~10M ALU + ~2.5M hash lookups per frame. Trivial on any GPU
  built after 2018.
- The bottleneck is bandwidth (vertex output buffer), not the math.

---

## References

- R. Bridson et al., *Curl-Noise for Procedural Fluid Flow*, SIGGRAPH
  2007 — divergence-free noise.
- A. Barr, *Global and Local Deformations of Solid Primitives*,
  SIGGRAPH 1984 — Twist, Bend, Taper canonical definitions.
- T. Sederberg, S. Parry, *Free-Form Deformation of Solid Geometric
  Models*, SIGGRAPH 1986 — FFD original formulation.
- S. Worley, *A Cellular Texture Basis Function*, SIGGRAPH 1996 —
  Voronoi noise.
- I. Quílez, *Voronoi: distance to border*, https://iquilezles.org/
  articles/voronoilines/ — efficient 3D Voronoi in GLSL/MSL.
- I. Quílez, *Gradient noise (analytical)*, https://iquilezles.org/
  articles/gradientnoise/ — perlin-style gradient noise we use as the
  scalar field for curl.

---

## Files

- `MeshDeformationLayer.h`   — class + Op struct + OpType enum.
- `MeshDeformationLayer.cpp` — implementation + ImGui inspector.
- `deformers.metal.inc`      — MSL functions + dispatcher.
- `INTEGRATION.md`           — how to wire into the structure shader
                                and the uniform block.
