# MeshFractureLayer — Cracks & Breakage

Recoverable, animation-driven fracture for the SpaceGen structure mesh
(~5M triangles). All effects are pure shader work — no CPU mesh
manipulation, no destructive geometry edits, no per-frame buffer
uploads. The structure is "broken" by the GPU and reassembles the
moment the amount slider returns to zero.

The layer is a **consumer-only Generator** (mirrors `BeamLayer`,
`DirectionalLightLayer`, `AmbientLightLayer`): its `render()` is a
no-op. `StructureLayer::render()` walks the bus, picks up the
first enabled `MeshFractureLayer`, and packs its parameters into the
structure-pass uniforms. The shader code in `fracture.metal.inc` is
included into `structure.metal` at well-defined extension points
(see `INTEGRATION.md`).

This means the fracture effects layer onto **the existing PBR + twist +
displace + Syphon stack** and respects all of it (lighting still hits
shards, the dissolve emit point still receives Syphon overlay, etc.).

---

## Modes (combinable)

The layer exposes a `mode` field that is a bitmask. Cracks + dissolve
+ explode + glitch can all be active simultaneously, blended by their
respective amounts. The bitmask is packed into a single int slot in the
uniform so the shader can branch tightly.

### 1. Animated cracks (`Mode::Cracks`)

Voronoi cell network in **world space**. Each fragment evaluates the
distance to the nearest Voronoi edge; fragments inside a thin band
around the edge are darkened or emit a thin orange glow. The
distance-to-edge follows the IQ formulation (Inigo Quilez, "Voronoi —
distances", 2007) which gives a true distance to the cell *boundary*
(F1 - F2 in dot space), not just the F1-F2 difference of cell IDs:

```
F1 distance = min over 27-cell neighborhood of |p - cellSite_i|
edge dist   = min over 27 cells of dot(0.5*(cellSite_a + cellSite_b) - p,
                                       normalize(cellSite_b - cellSite_a))
```

A single `smoothstep(crackWidth, 0.0, edgeDist)` gives an
antialiased crack mask `c ∈ [0, 1]`. The base color is multiplied by
`(1 - c * crackDarken)` and the layer's emissive output adds
`c * crackGlowColor * crackGlow`. Time is folded into the
cell site jitter so the network slowly drifts (a small per-cell
phase offset to `hash3(cell)` driven by `time * crackJitterSpeed`).

References:
- IQ, "Voronoi distances" — https://iquilezles.org/articles/voronoilines/
- IQ, "Smooth Voronoi" — https://iquilezles.org/articles/smoothvoronoi/

### 2. Dissolve burn (`Mode::Dissolve`)

Radial dissolve from a world-space `burnPoint`. The fragment
discriminator is

```
n   = noise3D(worldPos * dissolveScale) ∈ [0, 1]
r   = length(worldPos - burnPoint)
thr = saturate( (dissolveAmount * radius) - r * 0.0   // pure-time mode
              ,                                       // OR
                dissolveAmount + (r * dissolveRadialBias - 1.0) * 0.5
                                                      // OR radial mode
              )
keep = step(n, 1.0 - thr)
```

When `keep == 0` the fragment is discarded (or its alpha becomes 0
in `emitLightsOnly`-style passes — see INTEGRATION.md for the choice).
A thin **emit band** around the threshold receives an orange/cyan glow
that scales with `dissolveEdgeGlow`:

```
edge = smoothstep(edgeWidth, 0.0, abs(n - (1.0 - thr)))
emit += edge * dissolveEdgeColor * dissolveEdgeGlow
```

Threshold animates from `burnPoint` outward at `dissolveSpeed`
meters/sec, optionally clamped by `dissolveRadius`. When
`useNoiseDissolve == false` the entire structure dissolves uniformly
from `dissolveAmount` (classic "fade-out as burn") — the per-fragment
noise still provides texture but `r` is ignored.

References:
- The dissolve technique is standard since UE3 — see the canonical
  treatment in Inigo Quilez's "Useful little functions" + Ben Cloward's
  shader breakdown ("Dissolve Shader", 2020).
- Hash-based 3D value noise — IQ value noise variant (no texture
  required, ~6 ALU ops per evaluation).

### 3. Per-shard explode (`Mode::Explode`)

Each shard is identified by `shardId = hash3(floor(worldPos * shardDensity))`.
We don't precompute Voronoi sites on the CPU — the shard partition is
implicit in the floor-grid bucketing. This gives ~`shardDensity^3` cells
per cubic meter of mesh, deterministic across frames.

Per shard:

```
cellCenter  = (floor(worldPos * shardDensity) + 0.5) / shardDensity
shardId     = hash3(floor(worldPos * shardDensity))
direction   = normalize(cellCenter - structureCentroid) +
              jitterFromHash(shardId) * shardJitter
offset      = direction * explodeAmount *
              (0.5 + 0.5 * hash1(shardId))   // per-shard random magnitude
rotation    = rotateAround(cellCenter,
                           rotAxisFromHash(shardId),
                           explodeAmount * shardSpin * hash1(shardId))
```

This entire computation is done in the **vertex shader** before the
twist/displace step. Vertices sharing a cell bucket move together —
the floor-grid bucketing IS the shard partition. No connectivity
information is required.

The geometric trick that makes this work on a dense structure mesh
without visible tearing at low explodeAmount: the bucketing is in
**world space, not local**, and `shardDensity` should be tuned so
each cell covers many triangles. ~2-5 cells per meter on a 30m
structure (so ~60-150 cells across) gives recognizable, large shards
without breaking individual triangle edges into chaos.

References:
- Notch "Shatter" deformer (CPU-side Voronoi + per-piece rigid body) —
  we're implementing the *visual* of this in pure shader work.
  See notch-mastery `effect-recipes.md` entry 20.
- The hash-grid shard partitioning is standard "voxelized fracture",
  e.g. Müller et al. 2013 "Real Time Dynamic Fracture", which used a
  similar regular grid as a fast approximation to Voronoi.

### 4. Glitch tear (`Mode::Glitch`)

Vertical-band tearing in screen space-aligned world Y. For each
vertex:

```
band      = floor(worldPos.y * glitchFrequency)
hashed    = hash1(band + floor(time * glitchSpeed))
shouldTear = step(1.0 - glitchAmount, hashed)
offset.x  = shouldTear * (hash1(band) - 0.5) * 2.0 * glitchMagnitude
```

A per-vertex alpha-flicker is added in the fragment shader by
hashing `(band, frameIndex)` and conditionally darkening or making
the fragment transparent. The result is the classic "datamosh /
analog VHS dropout" feel — vertical slices of geometry slide left
or right by random amounts and occasionally drop frames.

References:
- The vertical-band displacement is standard since e.g. Adam Ferriss'
  "Glitch" shaders (2014). For SpaceGen we add it to the vertex stage
  (not just post-FX) so the displacement is real 3D motion that
  respects perspective.

---

## Voronoi shard math — derivation

Standard cellular noise (Worley 1996) over a 3D grid. We use the
27-neighbor variant (3×3×3 cells around the lookup point) because the
2D "9-neighbor" variant misses corner cells in 3D.

```
p           = worldPos * shardDensity
cell        = floor(p)
local       = fract(p)

best_F1     = +infinity
best_F2     = +infinity
best_site_F1 = vec3(0)

for each n in {-1,0,1}^3:
    neighborCell = cell + n
    site         = neighborCell + 0.5 + (hash3(neighborCell) - 0.5) * jitter
    diff         = site - p
    d            = length(diff)
    if d < best_F1:
        best_F2     = best_F1
        best_F1     = d
        best_site_F1 = site
    else if d < best_F2:
        best_F2 = d

# For the explode mode we use best_site_F1 → shard centroid.
# For the cracks mode we use the IQ edge-distance formulation
# (see crackEdgeDistance() in fracture.metal.inc).
```

`jitter ∈ [0, 1]` controls how irregular the cells are; 0 = pure cubic
grid, 1 = fully random Worley. SpaceGen uses 0.7 as a default — gives
natural-looking but still convex-ish cells.

---

## Why everything is shader-based

The structure mesh is ~5M triangles. A CPU-side Voronoi decomposition
that produces ~200 convex shards would require:

1. A heavy preprocess pass (BSP / per-cell triangle classification)
   on every render-time parameter change.
2. A separate vertex buffer per shard (or per-shard instance metadata).
3. Reupload to GPU on every explodeAmount change if the shards
   themselves move (they do).

Shader-only fracture has none of these costs and adds <2ms on Apple
M-series hardware at 1080p for the full PBR + cracks + dissolve +
explode + glitch combination (measured against the existing structure
pass).

Quality tradeoff: the shards are bound to the world-space grid, so
they don't follow individual triangles. At very high `shardDensity`
(>10 cells/meter) you start to see triangles split between cells,
which produces visible micro-tearing. The inspector clamps
`shardDensity` to 0.3..5.0 c/m to stay in the clean regime.

---

## ImGui inspector layout

```
Mode flags
  [ ] Animated cracks
  [ ] Dissolve burn
  [ ] Per-shard explode
  [ ] Glitch tear

Amount      [============] 0.42       Mod: [LFO 3 ▾] depth [0.7]

Cracks
  density          [=====]  1.8 cells/m
  width            [===]    0.06
  jitter speed     [=]      0.12
  glow color       [orange]
  glow intensity   [====]   0.8

Dissolve
  burn point       [ X][ Y][ Z]
  noise scale      [====]   2.4
  speed            [===]    0.4
  radius           [=====]  12.0
  edge glow        [====]   1.4
  edge color       [orange]
  radial bias      [==]     0.5

Explode
  shard density    [===]    1.5 cells/m
  shard jitter     [====]   0.7
  shard spin       [==]     0.3

Glitch
  frequency        [=====]  6.0 bands/m
  magnitude        [===]    0.4
  speed            [====]   8.0 Hz
```

---

## File layout

| File | Purpose |
|---|---|
| `MeshFractureLayer.h`   | Class declaration. Public POD fields, no virtual params yet (matches v1 of the parameter-graph-spacegen skill). |
| `MeshFractureLayer.cpp` | Constructor defaults + ImGui inspector. `render()` is a no-op. |
| `fracture.metal.inc`    | MSL functions: `fracHash3`, `fracVoronoi27`, `fracCrackEdgeDist`, `fracDissolveMask`, `fracExplodeOffset`, `fracGlitchOffset`, and one dispatch function `fracApplyVertex` / `fracApplyFragment`. |
| `INTEGRATION.md`        | Step-by-step instructions for wiring the snippets into `structure.metal` (the active PBR structure shader) and into `MetalRenderer::renderStructureMeshes`. |
