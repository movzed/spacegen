// =============================================================================
// Tier 2 — Stretch Heatmap MSL snippet
// =============================================================================
//
// Inserts into the existing fs_main in MetalRenderer.cpp:
//   • After the syphonSample computation
//   • Before the lighting integration / final composition
//
// Reads:
//   in.uv          — selected UV layer (UV0 or UV1 per the auto-hybrid rule)
//   in.worldPos    — world-space position
//   u.modeFlags.y  — new uniform: 1.0 enables the heatmap, 0.0 disables
//   u.modeFlags.z  — selects energy formula: 0 = stretch ratio,
//                                            1 = symmetric Dirichlet density
//
// Writes:
//   When enabled, REPLACES the per-fragment baseColor with a viridis-like
//   color encoding the distortion at that fragment. All lighting still runs
//   on top so dark regions of the structure stay dark — the operator sees
//   the heatmap modulated by light, like any other material channel.
//
// Drop this in just BEFORE the `float3 baseMat   = u.baseColorRoughness.rgb`
// line in fs_main:

if (u.modeFlags.y > 0.5) {
    // Jacobian of (uv → world): 3×2 matrix [du/dx_uv  du/dy_uv].
    // We use screen-space derivatives of in.uv and in.worldPos as a
    // proxy — works for any continuous parameterization, including
    // the UV0/UV1 hybrid the engine picks per fragment.
    float3 dWx = dfdx(in.worldPos);
    float3 dWy = dfdy(in.worldPos);
    float2 dUx = dfdx(in.uv);
    float2 dUy = dfdy(in.uv);

    // Construct the 2×2 matrix M = J^T J. Its eigenvalues are σ₁², σ₂²:
    //   M = transpose(J) * J
    // where J is approximated as:
    //   J ≈ [dWx, dWy] * inv([dUx, dUy])
    // The eigenvalues of J^T J equal those of (dW·dWT) · inv(dU·dUT).
    // Simpler: project dWx, dWy into UV space by solving the linear
    // system (dUx, dUy) (a, b)^T = (dWx, dWy)^T per axis.

    float det = dUx.x * dUy.y - dUx.y * dUy.x;
    if (abs(det) < 1e-12) {
        // Degenerate UV (no gradient). Show pure red — broken region.
        baseTex = float4(1.0, 0.0, 0.0, 1.0);
    } else {
        float  invDet = 1.0 / det;
        float3 J_u = ( dUy.y * dWx - dUx.y * dWy) * invDet;  // ∂world/∂u
        float3 J_v = (-dUy.x * dWx + dUx.x * dWy) * invDet;  // ∂world/∂v

        // Form M = J^T J (2×2 symmetric):
        float a = dot(J_u, J_u);
        float b = dot(J_u, J_v);
        float c = dot(J_v, J_v);

        // Eigenvalues of [[a,b],[b,c]]:
        //   λ = (a+c)/2 ± sqrt(((a-c)/2)^2 + b^2)
        float tr   = a + c;
        float diff = (a - c) * 0.5;
        float disc = sqrt(max(diff * diff + b * b, 0.0));
        float l1 = max(tr * 0.5 + disc, 1e-12);   // σ₁²
        float l2 = max(tr * 0.5 - disc, 1e-12);   // σ₂²
        float s1 = sqrt(l1), s2 = sqrt(l2);       // σ₁, σ₂

        // Choose distortion metric:
        //   modeFlags.z = 0 → stretch ratio   max(σ₁/σ₂, σ₂/σ₁); ideal 1
        //   modeFlags.z = 1 → symmetric Dirichlet density; ideal 4
        float distortion;
        if (u.modeFlags.z > 0.5) {
            distortion = (l1 + l2 + 1.0/l1 + 1.0/l2);
        } else {
            distortion = max(s1 / max(s2, 1e-6), s2 / max(s1, 1e-6));
        }

        // Map distortion → viridis-ish ramp.
        // Ideal value depends on the metric; normalize to a 0..1 stress.
        float ideal = (u.modeFlags.z > 0.5) ? 4.0 : 1.0;
        float t = saturate((distortion - ideal) / 4.0);   // 0..1

        // Viridis-like ramp (cheap analytical fit):
        float3 ramp = mix(mix(mix(
            float3(1.00, 1.00, 1.00),   // 0.00 white  (perfect)
            float3(0.20, 0.85, 0.95),   // 0.25 cyan
            saturate(t / 0.25)),
            float3(0.30, 0.85, 0.30),   // 0.50 green
            saturate((t - 0.25) / 0.25)),
            float3(0.95, 0.30, 0.20),   // 1.00 red    (broken)
            saturate((t - 0.50) / 0.50));
        baseTex = float4(ramp, 1.0);
    }
    // The shader continues with normal lighting — the heatmap becomes the
    // baseColor and gets lit just like the regular material. To get a
    // flat unlit view, set ambient = 1, all light layers off.
}
