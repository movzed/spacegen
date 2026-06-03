#pragma once
// MeshDecimate — runtime LOD generator for SpaceGen.
//
// Wraps meshoptimizer's quadric-error simplifier to let the operator pick
// mesh density in the UV Analysis panel. Lower density → faster xatlas /
// SLIM at the cost of visible detail. The estimator below extrapolates
// observed xatlas runtimes so the UI can show "ETA: ~3 min" before the
// operator commits.
//
// Reference: github.com/zeux/meshoptimizer (Arseny Kapoulkine, MIT).

#include "Scene.h"

namespace spacegen {

struct DecimateResult {
    bool        ok = false;
    std::string error;
    int         trianglesBefore = 0;
    int         trianglesAfter  = 0;
    double      elapsedSec      = 0.0;
    float       lodError        = 0.0f;   // 0..1, max reported by meshopt
};

// Decimate `mesh` to roughly `targetTriangleRatio` × original triangle
// count. Modifies indices in place; vertex positions/normals/uvs are
// left as-is (meshopt's simplifier reuses existing vertices, so the
// position array only contains "used vertices" after decimation, but
// unused entries are harmless at runtime — only VS reads them).
//
// Uses meshopt_simplifyWithAttributes so UV0 (the artist's PRT_UVW
// unwrap) is included in the error metric — UV-rich regions (the
// central mask) are protected from over-decimation.
//
// `ratio` of 1.0 → no decimation, returns immediately.
// `ratio` <= 0.0 → returns error.
DecimateResult decimateMesh(MeshData& mesh, float ratio);

// ============================================================
// UV-bake-only proxy pipeline (operator-correct architecture)
// ============================================================
//
// SpaceGen renders the DENSE mesh — every triangle is needed for
// silhouette, lighting integration, beam intersection, etc. We never
// want to throw away geometry for visual reasons. But xatlas + SLIM
// scale superlinearly with triangle count and choke on >2 M tris.
//
// Solution: decimate ONLY for the UV bake. Build a coarser proxy in
// memory, run xatlas + SLIM on the proxy, then SCATTER the resulting
// uvs1 back to the dense mesh via barycentric interpolation. The
// dense mesh in scene.meshes[0] keeps its original positions /
// normals / indices / uvs untouched — only its uvs1 is overwritten
// with the transferred values.

// Build a proxy mesh for UV baking. Returns a new MeshData with
// decimated positions/indices, normals copied through, original uvs
// preserved (passed to meshopt as attribute for quadric-error term).
// uvs1 is left empty. The proxy is independent from the source — the
// caller owns it and feeds it to xatlas/SLIM.
MeshData buildUvBakeProxy(const MeshData& dense, float ratio);

// Transfer per-vertex uvs1 from a proxy mesh back to a dense mesh.
//
// For each dense vertex:
//   1. Find the closest triangle on the proxy (in 3D world space).
//   2. Compute the dense vertex's barycentric coordinates on that
//      triangle's plane (clamped to the triangle).
//   3. denseOut[i] = bary.a * proxy.uvs1[T.v0] +
//                    bary.b * proxy.uvs1[T.v1] +
//                    bary.c * proxy.uvs1[T.v2]
//
// `denseOut` is resized to dense.positions.size(). The 3D-space
// nearest-triangle search uses a uniform spatial grid for O(N) total
// cost on million-vertex meshes.
//
// `progressCb` is optional — called with (0..100) during the dense
// vertex sweep. Returning false aborts.
struct TransferResult {
    bool        ok = false;
    std::string error;
    int         denseVerts   = 0;
    int         proxyTris    = 0;
    double      elapsedSec   = 0.0;
};

using TransferProgressFn = bool(*)(int progressPct, void* user);

TransferResult transferUv1FromProxyToDense(
    const MeshData& dense,
    const MeshData& proxyWithUv1,
    std::vector<glm::vec2>& denseOutUv1,
    TransferProgressFn cb = nullptr,
    void* user = nullptr);

// ============================================================
// Camera-projected UV1 bake
// ============================================================
//
// For projection-mapping setups where the scene camera == the physical
// projector POV, this is the simplest, most direct UV layout: each
// vertex gets uv1 = NDC(camera_projection * camera_view * worldPos),
// remapped to [0, 1]² with the standard image y-flip.
//
// Behaviour:
//   - The texture STAYS BAKED to the surface (it's a regular UV1 buffer).
//   - Each face of the structure shows the rectangular slice of the
//     1920×1080 source video that corresponds to its screen-space
//     position at the moment of bake.
//   - A flat wall facing the camera shows its corresponding pixel rect.
//   - Vertices behind the camera (w ≤ 0) get uv1 = (-1, -1) so the
//     sampler returns 0 outside the frustum — those faces stay dark
//     (correct: a real projector can't light them either).
//
// Computes uv1 for every vertex of `dense` and writes into `outUv1`
// (resized to dense.positions.size()). No xatlas / SLIM / transfer
// needed — it's literally a per-vertex projection. Fast: O(N) over
// vertices, milliseconds even on 5 M vertex meshes.
//
// `projection` and `view` are the matrices that the engine uses for
// the structure pass (same ones in scene.camera).
void bakeCameraProjectedUv1(const MeshData& dense,
                             const glm::mat4& projection,
                             const glm::mat4& view,
                             std::vector<glm::vec2>& outUv1);

// Empirical estimate of xatlas + SLIM total wall time on the user's
// M1 Max for a mesh of `triangleCount` triangles.
//
// Xatlas runtime does NOT scale smoothly with N. The dominant factors
// are mesh quality (zero-area faces, zero-length edges, non-manifold
// regions) and Sharp-Edge pre-cut boundary count — neither of which
// is knowable from triangle count alone. So we report a RANGE rather
// than a misleading point estimate.
//
// Calibrated from three observed runs:
//   - 0.5 M clean tris             → 30 s
//   - 2.0 M tris (with 30k+ degens)→ 7200 s (2 h, observed)
//   - 7.7 M raw tris               → ~9000 s @ 51% (extrapolated)
//
// `lowSec`  = optimistic case (cleanish mesh, few sharp edges).
// `highSec` = pessimistic case (degenerate-heavy mesh).
struct AtlasTimeRange {
    double lowSec  = 0.0;
    double highSec = 0.0;
};
AtlasTimeRange estimateAtlasTimeRange(int triangleCount);

// Backward-compatible scalar — returns the optimistic estimate.
double estimateAtlasTimeSeconds(int triangleCount);

} // namespace spacegen
