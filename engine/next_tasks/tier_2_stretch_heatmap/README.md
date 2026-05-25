# Tier 2 — Stretch Heatmap Diagnostic

Visualises UV distortion as a per-fragment color overlay on the structure.
Lets the operator see *where* the UV bake is healthy and where it's
stretching the video.

## What it shows

For each fragment, we compute the singular values σ₁ ≥ σ₂ of the Jacobian
of the (UV → world) mapping using screen-space derivatives:

```
J = [ ∂worldPos/∂uv.x  ∂worldPos/∂uv.y ]    (3×2 matrix)
σ₁, σ₂ = singular values of J
```

We don't need explicit SVD — the eigenvalues of `J^T J` (a 2×2 matrix)
give us σ₁² and σ₂². The relevant ratios:

- **Stretch ratio**: `max(σ₁/σ₂, σ₂/σ₁)` — ideal = 1.0 (isometric).
- **Symmetric Dirichlet energy density**: `σ₁² + σ₂² + σ₁⁻² + σ₂⁻²` —
  ideal = 4. This is also what SLIM (Tier 3) minimizes.

The heatmap maps either of those scalars onto a viridis-like ramp:

```
   1.0  → white (perfect)
   1.5  → cyan
   2.0  → green
   3.0  → yellow
   5.0+ → red (broken)
```

Operator gets an instant visual answer to *"should I trust this atlas?"*.

## Files

- `StretchHeatmap.metal.inc` — MSL snippet to drop into the existing
  fragment shader; reads `in.uv` + `in.worldPos`, computes the ratio,
  outputs the color.
- `panel_toggle.snippet.mm` — ImGui block for the UV Analysis panel
  ("Show stretch heatmap" checkbox + ramp legend).

## Integration

See `INTEGRATION.md`. Minimal: ~10 lines added to `MetalRenderer.cpp`'s
fragment shader inside a new `if (u.modeFlags.y > 0.5) { ... }` branch,
one new uniform field, one CPU mirror, one panel checkbox. Total wall
time: 30 minutes.
