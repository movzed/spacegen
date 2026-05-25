# Next Tasks — UV Quality Roadmap (ALL TIERS LANDED)

Status as of commits `8141a3c` / `a09a902` / `<tier3>` / `<tier4>`.

This folder originally held drop-in deliverables for the post-Tier-1 UV
pipeline. **All tiers up to Tier 4 are now integrated into the engine.**
The folder is kept for historical reference of the design + integration
plans; the live code lives in `engine/core/` and `engine/gui/`.

## Final status

| Tier | Folder                          | Engine location                                                          | Status            |
|------|---------------------------------|--------------------------------------------------------------------------|-------------------|
| 1    | (was inline)                    | `engine/core/UvAtlas.cpp::preCutOnSharpEdges`                            | **Landed**        |
| 2    | `tier_2_stretch_heatmap/`       | `engine/backends/metal/MetalRenderer.cpp` + panel                        | **Landed**        |
| 3    | `tier_3_spacegen_optimize/`     | `engine/core/UvOptimize.{h,cpp}` (Eigen)                                 | **Landed**        |
| 4    | `tier_4_geogram_swap/`          | `engine/core/UvAtlasGeogram.{h,cpp}` (opt-in via cmake flag)             | **Landed (opt-in)** |
| 5    | —                                | RizomUV C++ SDK                                                          | **Deferred — no macOS support** |

## Operator-facing controls (UV Analysis panel)

```
Atlas engine:        [xatlas (default)            ▼]
                     [geogram (Spectral LSCM)        ]   ← requires
                                                          -DSPACEGEN_ENABLE_GEOGRAM=ON

☑ Sharp Edges pre-cut (Tier 1)
  Dihedral threshold: [────●──────────] 35.0°

☑ SpaceGen Optimize SLIM refinement (Tier 3)
  Iterations:         [─●────] 12
  ☑ Projection-aware weighting
  View weight exponent: [──●───] 1.50
  Back-face floor:      [●─────] 0.10

──────────────────────────────────────
Stretch heatmap diagnostic
☐ Show stretch heatmap
  (when ON: full diagnostic view, lighting bypassed)

──────────────────────────────────────
Atlas applied: YES (UV1 = xatlas)            ← green when live atlas loaded
Cache: examples/<scene>/uv1_atlas.bin
       358 MB on disk
[ Apply atlas UVs now (hot-reload, no restart) ]

──────────────────────────────────────
[ Generate UV atlas with xatlas ]
  Stage 6/6 — SpaceGen Optimize (SLIM refining)
  ▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓░░░░░░  72%   (47.3s)
  [ Cancel ]
```

## Tier 5 — why it's deferred

- **RizomUV C++ SDK** is licensed by Rizom-Lab, requires annual fee + per-seat costs.
- Platform support listed at <https://www.rizomuv.com/c-library/>:
  Windows 10/11 (Visual Studio 2015+) and Linux (Red Hat 8.6+, Rocky 9+).
- **No macOS build**. SpaceGen v1 targets macOS Apple Silicon — the SDK
  cannot be linked at all in the current configuration.

Options if Tier 5 becomes essential later:

1. **Split pipeline**: run RizomUV on a Linux build farm / CI container
   as an asset-prep step. SpaceGen engine consumes the precomputed
   atlas (`uv1_atlas.bin`). Adds infrastructure overhead but stays
   macOS-only at runtime.
2. **Negotiate custom macOS port** with Rizom-Lab. Expensive and uncertain.
3. **Skip indefinitely**. Tier 1 + Tier 3 + Tier 4 cover ≥95% of the
   visual quality gap to RizomUV for projection-mapping use cases.

Recommendation: **option 3**. Only revisit if a specific production job
demonstrably needs Rizom-quality and the budget supports option 1.

## Decision tree (post-implementation)

```
Need higher-quality UVs?
├─ Try Tier 1+3 first (default in the panel).
│  ├─ Visually acceptable? → Done.
│  └─ Visible defects?     → Open Tier 2 heatmap to localise the problem.
│     ├─ Localised stretch  → Increase Tier 3 iterations or projection-aware
│     │                       exponent.
│     └─ Chart layout bad  → Enable Tier 4 (rebuild with geogram).
│        ├─ Tier 4 fixed it? → Ship.
│        └─ Still bad?       → Tier 5 is the only remaining option, and
│                              it's deferred. Consider option 1 above.
```

## Folder contents

Original design + reference material kept verbatim:

```
tier_2_stretch_heatmap/
├── README.md                 — algorithm explanation
├── StretchHeatmap.metal.inc  — the MSL snippet we landed in fs_main
├── panel_toggle.snippet.mm   — the ImGui block we landed in the panel
└── INTEGRATION.md            — exact integration steps (now historical)

tier_3_spacegen_optimize/
├── README.md                 — algorithm pedigree + projection-aware twist
├── UvOptimize.h              — historical copy (engine/core/UvOptimize.h is canonical)
├── UvOptimize.cpp            — skeleton stub (engine/core/UvOptimize.cpp is canonical)
└── INTEGRATION.md            — exact integration steps (now historical)

tier_4_geogram_swap/
└── README.md                 — decision criteria + license check
```
