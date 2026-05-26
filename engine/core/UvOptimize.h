#pragma once
// SpaceGen Optimize — projection-aware SLIM refinement of a UV layer.
//
// Take a mesh whose UVs are already roughly correct (e.g. fresh from
// xatlas with Tier-1 Sharp Edges pre-cut) and run a SLIM iteration on
// each chart to minimise symmetric Dirichlet energy. The energy is
// weighted by per-face visibility from the scene camera so the
// optimiser prioritises the surfaces the audience actually sees.
//
// Math reference:
//   Rabinovich, Poranne, Panozzo, Sorkine-Hornung,
//   "Scalable Locally Injective Mappings", SIGGRAPH 2017.
//
// Our additions:
//   - Per-face visibility weight `vis_t = saturate(dot(N_t, -view_dir))`
//     baked into the per-triangle term of the symmetric Dirichlet energy.
//   - Anchor constraint on chart-boundary vertices so the xatlas packing
//     stays valid (UVs inside [0,1] without inter-chart overlap).
//
// Threading: thread-safe with respect to the caller. The internal SLIM
// loop runs on the caller's thread. Per-chart parallelism via std::async
// is encapsulated and bounded by `opts.maxWorkerThreads`.

#include "Scene.h"        // for MeshData

#include <glm/glm.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace spacegen {

struct UvOptimizeOptions {
    // ---- SLIM core ----
    int   maxIterations         = 20;     // safety cap; usually converges in 8-12
    float convergenceTol        = 1e-4f;  // ΔE/E threshold for early-exit
    float boundaryAnchorWeight  = 1e6f;   // soft Dirichlet constraint on boundary
    bool  flipPreventionEnabled = true;   // line-search step rejecting any flip
    float epsSingularValue      = 1e-7f;  // floor for σ when computing weight

    // ---- Projection-aware weighting (our addition) ----
    bool        projectionAware  = true;
    glm::vec3   viewDir          = glm::vec3(0.0f, -1.0f, 0.0f);
    // Visibility weight `vis_t` is `pow(max(dot(N_t, -viewDir), 0), visExponent)`,
    // then mixed with a floor so even back-facing tris still get some weight.
    float       visExponent      = 1.0f;
    float       visFloor         = 0.10f; // never weight below this (back-faces)

    // ---- Performance ----
    int   maxWorkerThreads       = 0;      // 0 = autodetect (hw_concurrency)
    int   minChartTriangleCount  = 64;     // skip tiny charts (cheap, low quality gain)
    int   maxChartsRefined       = 8000;   // hard cap; SLIM allocates ~MB/chart of
                                            // Eigen workspace — 218k charts OOM'd a
                                            // 32 GB box. When chart count > cap, we
                                            // refine the largest N charts (by tri
                                            // count) and leave the rest as xatlas
                                            // output (already decent).
};

struct UvOptimizeResult {
    bool                  ok = false;
    std::string           error;
    int                   chartsOptimised = 0;
    int                   chartsSkipped   = 0;
    double                initialEnergy   = 0.0;   // total symmetric Dirichlet
    double                finalEnergy     = 0.0;
    double                elapsedSeconds  = 0.0;
    std::vector<glm::vec2> refinedUVs;             // same size as mesh.uvs1
};

// Per-stage progress callback (analogous to UvAtlas::ProgressFn).
using OptimizeProgressFn =
    bool(*)(int progressPct, const char* stageName, void* user);

// Run SpaceGen Optimize on the UV1 layer of `mesh`. UV0 (PRT_UVW) and the
// mesh topology are untouched. Returns a new uvs1 vector (same size as
// the input) plus diagnostics.
//
// Pre-conditions:
//   - mesh.uvs1.size() == mesh.positions.size() (xatlas output, or any
//     other UV1 producer that has already run).
//   - mesh.indices.size() % 3 == 0.
//
// Post-conditions:
//   - result.refinedUVs[i] is the optimised UV1 for vertex i.
//   - No triangle has flipped (signed UV-space area sign preserved).
//   - finalEnergy <= initialEnergy (monotonic SLIM).
UvOptimizeResult optimiseUVs(const MeshData& mesh,
                              const UvOptimizeOptions& opts = {},
                              OptimizeProgressFn cb = nullptr,
                              void* user = nullptr);

} // namespace spacegen
