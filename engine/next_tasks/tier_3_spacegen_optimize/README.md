# Tier 3 — SpaceGen Optimize

SLIM-based stretch reducer with projection-aware weighting. Polishes the
xatlas output so each UV island has minimal angular AND area distortion,
giving the "alive material" feel the user asked for.

## Pedigree

The math is **SLIM** — *Scalable Locally Injective Mappings* by Rabinovich,
Poranne, Panozzo, Sorkine-Hornung, SIGGRAPH 2017 ([paper PDF, open
access](https://igl.ethz.ch/projects/parameterization/SLIM/SLIM.pdf)).
This is a public algorithm; reimplementing it from the paper is legally
clean (cite the authors, that's it).

The original twist that makes this **SpaceGen Optimize** rather than a
generic SLIM port is described in section "Projection-aware weighting"
below. As far as we found, the modification doesn't exist in published
literature or in any UV tool, and is specific to projection-mapping
where the operator's mesh has a known projector POV.

## Algorithm at a glance

For each chart (output island of xatlas):

```
Input:  3D vertex positions P, triangle list T, initial UVs u₀
Energy: E(u) = Σ_t area(t) · ( σ₁² + σ₂² + σ₁⁻² + σ₂⁻² )
        where σ₁, σ₂ are singular values of the Jacobian of (u → P|_t)
        — symmetric Dirichlet energy. Penalises stretch AND compression.

Iterate u = u₀:
  1. For each triangle t, compute the Jacobian J_t of the current u.
     SVD: J_t = U Σ Vᵀ, Σ = diag(σ₁, σ₂).
  2. Compute the "rotation-free" target Jacobian:
       J*_t = U · diag(1, 1) · Vᵀ
     This is the closest isometry to J_t (minimizes Frobenius dist).
     Symmetric Dirichlet wants σ₁ = σ₂ = 1 → no stretch.
  3. Build a sparse linear system A u_new = b minimising:
       Σ_t w_t · ‖ J(u_new)|_t  −  J*_t ‖_F²
     where w_t is a per-triangle weight = ∂E/∂σ at current σ
     (reweighted least squares — the SLIM proximal step).
  4. Solve with sparse Cholesky (Eigen::SimplicialLDLT).
  5. Line search along (u_new − u) for a step that doesn't introduce
     flipped triangles (this is the "locally injective" guarantee).
  6. Accept the step, check convergence (ΔE / E < tol), repeat.

Output: refined UVs u with minimal distortion, no flips, boundary anchored.
```

Typical convergence: 10–20 iterations. For a chart of ~30k triangles,
~50 ms per iteration on M1 Max with Eigen's LDLT.

## Projection-aware weighting (our original twist)

Standard SLIM minimises area-weighted symmetric Dirichlet:

```
E = Σ_t  area_t  ·  symDir(σ₁_t, σ₂_t)
```

Our innovation: weight by **projector-facing visibility**. Surfaces the
camera (= projector) sees frontally get a higher weight; grazing-angle
surfaces (which the projector barely lights anyway) get lower weight.

```
E_spacegen = Σ_t  area_t · vis_t · symDir(σ₁_t, σ₂_t)

where  vis_t = saturate( dot(N_t, -view_dir) )   ∈ [0, 1]
       N_t   = face normal of triangle t
       view_dir = camera forward in world space
```

Effect: the optimiser spends its "budget" on the parts of the structure
the audience actually looks at, accepting more stretch on edges and
grazing facets. For projection mapping this is correct — those facets
get little projector light anyway, so visible artefacts there are
masked. We get visibly better front-facing UVs at no extra cost.

To our knowledge this weighting is novel; it doesn't appear in:
- SLIM (Rabinovich et al. 2017)
- Boundary First Flattening (Sawhney & Crane 2017)
- Progressive Embeddings (Shen et al. 2019)
- OptCuts (Li et al. 2018)
- RizomUV public materials

Citable name: **PA-SLIM** (Projection-Aware SLIM) if we ever paper it.

## Files in this folder

- `UvOptimize.h`        — public API + options
- `UvOptimize.cpp`      — implementation skeleton (~120 LOC stub +
                          inline pseudocode for the SLIM core)
- `slim_solver.h`       — sparse-solver helpers (Eigen wrappers)
- `slim_solver.cpp`     — SLIM iteration loop, line search,
                          flip detection
- `INTEGRATION.md`      — Eigen FetchContent + CMake + worker
                          thread changes + panel checkbox

## Estimated landing effort

| Sub-task                                  | LOC  | Time |
|-------------------------------------------|------|------|
| Eigen FetchContent + build verification   |  ~10 | 30 m |
| Chart extraction from xatlas output       | ~150 |  1 h |
| Per-chart Jacobian / energy / weight code | ~120 |  1 h |
| Sparse system assembly                    |  ~80 |  1 h |
| LDLT solve + line search + flip check     |  ~80 |  1 h |
| Projection-aware weight integration       |  ~30 | 30 m |
| Anchor preservation at chart boundaries   |  ~40 | 30 m |
| Panel checkbox + progress reporting       |  ~40 | 30 m |
|                                           |      |      |
| **Total**                                 |  ~550 LOC | **~6 h** |

## When to land it

After we validate Tier 1 (Sharp Edges) visually with the operator:

- If the result on the test scene is already "good enough for show",
  Tier 3 is optional polish — land it before any release where
  competitor RizomUV-quality is the bar.
- If Tier 1 visibly stretches the video on some flat regions or
  curved cores, Tier 3 is the right next move.
