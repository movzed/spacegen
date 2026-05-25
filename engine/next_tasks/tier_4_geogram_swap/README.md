# Tier 4 — Geogram swap (nuclear option)

Replace xatlas with Bruno Lévy's Geogram library if Tiers 1+2+3 still
leave visible UV defects on the test scene. Lévy is one of the inventors
of LSCM; Geogram is the academic reference implementation.

## When to consider this

ONLY after:
- Tier 1 (Sharp Edges) landed and tuned.
- Tier 2 (heatmap) shows stretch concentrated in specific regions.
- Tier 3 (SpaceGen Optimize SLIM) reduces stretch but the chart layout
  itself is the problem (xatlas's chart segmentation is suboptimal for
  this mesh family).

If the layout is the issue, Geogram's `mesh_make_atlas()` uses:
- **Spectral LSCM**: replaces LSCM's free degrees of freedom with the
  Hessian's eigenvector — better balanced distortion.
- **ABF++**: nonlinear angle-based flattening, less stretch than LSCM
  on highly-curved charts.
- **Tetris packing**: original algorithm before xatlas; not always
  better but a different bin-packing heuristic worth A/B testing.

## Repo + license check

- GitHub: <https://github.com/BrunoLevy/geogram>
- License: **BSD 3-clause** (per the repo LICENSE file) — compatible.
- Size: ~150k LOC. Builds in ~3 minutes on M1 Max.
- Dependencies: nothing external (self-contained).

## Migration sketch

Geogram exposes parameterization through:

```cpp
#include <geogram/mesh/mesh.h>
#include <geogram/mesh/mesh_parameterization.h>
#include <geogram/mesh/mesh_atlas.h>

GEO::Mesh m;
m.facets.assign_triangle_mesh(positions, indices, true);
GEO::mesh_make_atlas(m, /*hard_angle=*/45.0, /*pack_method=*/PACK_TETRIS);
// uvs come back in m.facet_corners.attribute_named<double>("tex_coord")
```

That `hard_angle` parameter is Geogram's equivalent of our Tier 1 Sharp
Edges threshold. Good sign: same conceptual control surface.

## Decision criteria

| Indicator                                      | Action |
|------------------------------------------------|--------|
| Tier 1+3 hit ≥ 90% subjective acceptance       | Don't bother with Tier 4. |
| Stretch heatmap (Tier 2) shows ≥ 10% red       | Try Tier 4. |
| Chart count > 5000 after Tier 1                | Geogram's spectral LSCM may segment better. |
| Compile-time grows by > 2 min                  | Reject — too painful to develop with. |

## Files in this folder

This folder is intentionally **light**. Tier 4 is a "maybe never" — if
we get there, the integration is a CMakeLists swap + 1 wrapper file.
No code stub needed yet; the API sketch above is enough to remember
how to do it.
