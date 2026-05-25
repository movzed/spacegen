# Next Tasks — Staged UV Quality Roadmap

Drop-in deliverables for the post-Tier-1 UV pipeline. Each folder is
self-contained:

- A `README.md` explaining what the feature does and why it matters.
- Source files ready to copy into the engine.
- An `INTEGRATION.md` listing the exact edits to `CMakeLists.txt`,
  `Workstation.mm`, `MetalRenderer.cpp` etc.

Order of integration when needed:

| Tier | Folder | Status | Estimated work to integrate |
|------|--------|--------|------------------------------|
| 2    | `tier_2_stretch_heatmap/`    | Code complete, ready to drop in            | ~30 min                  |
| 3    | `tier_3_spacegen_optimize/`  | Design + skeleton + projection-aware twist | ~6 h to flesh out        |
| 4    | `tier_4_geogram_swap/`       | Research notes + decision criteria         | ~1-2 days if Tier 3 falls short |

## Why staged?

- **Tier 1** (already shipped in commit `8141a3c`): Sharp Edges pre-cut
  + big-chart xatlas. Addresses the seam-placement problem — the most
  visible defect for projection-mapping video.
- **Tier 2**: diagnostic — shows the operator a heatmap of stretch
  distortion overlaid on the structure. Cheap to integrate. Useful
  signal for deciding whether Tier 3 is needed at all.
- **Tier 3**: the killer feature — SLIM-based stretch reducer with
  projection-aware weighting. Polishes UV quality to RizomUV-class
  results. Heavy (Eigen dep, ~500 LOC) so only land it when we know
  it pays off.
- **Tier 4**: nuclear option — replace xatlas with Geogram (Bruno Lévy's
  reference parameterization library). Only if Tier 3 still leaves
  visible artifacts. Bigger dependency, longer build time.

## Decision tree

```
Tier 1 enabled and visually acceptable?
├─ Yes → Done. Ship.
└─ No  → Drop in Tier 2 heatmap to diagnose.
         ├─ Stretch concentrated in specific charts? → Tier 3 fixes it.
         └─ Stretch everywhere + chart layout bad?   → Tier 4 (geogram).
```
