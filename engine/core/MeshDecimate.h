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

// Empirical estimate of xatlas + SLIM total wall time on the user's M1
// Max for a mesh of `triangleCount` triangles, with our current options
// (Sharp Edges pre-cut at 50°, big-charts bias, bruteForce off, SLIM
// 12 iter).
//
// Fitted from two anchor points observed in production:
//   - 0.5 M tris → 30 s
//   - 7.7 M tris → 9000 s (extrapolated from 51% at 4500s)
// → t ∝ N^2.08 in millions of triangles. Adds a 20% overhead for
// SLIM + cache + hot-reload.
double estimateAtlasTimeSeconds(int triangleCount);

} // namespace spacegen
