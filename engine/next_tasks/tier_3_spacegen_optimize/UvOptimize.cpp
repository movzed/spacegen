// =============================================================================
// SpaceGen Optimize — implementation skeleton.
// =============================================================================
//
// This file is in next_tasks/ — NOT compiled by the engine yet. When you're
// ready to land Tier 3, move it into engine/core/, add it to CMakeLists.txt's
// source list, and wire it into the worker in Workstation.mm (see
// INTEGRATION.md).
//
// Status:
//   - Skeleton compiles standalone against Eigen + glm (no SpaceGen deps).
//   - The SLIM core is described in pseudocode comments inside
//     `runSlimOnChart()`. Translating it to live Eigen code is mechanical
//     once you read the SLIM paper section 4.
//   - The Projection-aware weighting (our novel addition) IS implemented
//     in full because it's small and self-contained.

#include "UvOptimize.h"

// Eigen will be added via FetchContent. Until then these includes are stubs;
// the implementation below is non-compiling until Eigen is wired.
// #include <Eigen/Sparse>
// #include <Eigen/SparseCholesky>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <future>
#include <thread>
#include <unordered_map>

namespace spacegen {

namespace {

// -----------------------------------------------------------------------------
// Geometric helpers
// -----------------------------------------------------------------------------

// Per-triangle 3D area.
inline float triArea3D(const glm::vec3& a, const glm::vec3& b,
                        const glm::vec3& c) {
    return 0.5f * glm::length(glm::cross(b - a, c - a));
}

// Per-triangle face normal (unit).
inline glm::vec3 triNormal(const glm::vec3& a, const glm::vec3& b,
                            const glm::vec3& c) {
    glm::vec3 n = glm::cross(b - a, c - a);
    float l = glm::length(n);
    return (l > 1e-12f) ? n / l : glm::vec3(0, 0, 1);
}

// Signed 2D area of a UV triangle. Sign flip indicates a fold.
inline float triSignedArea2D(const glm::vec2& a, const glm::vec2& b,
                              const glm::vec2& c) {
    return 0.5f * ((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x));
}

// 2×2 Jacobian J such that J · [u v]ᵀ = world-position increment for a
// triangle with given 3D edge frame and UV coords. Returned as a 2×3
// row-major float[6] mapping (Δu, Δv) → (Δx, Δy, Δz).
//
// Used to derive σ₁, σ₂ via SVD of J for the symmetric-Dirichlet energy.
//
// Implementation: build a per-triangle local 2D frame (e₁, e₂) aligned
// with the 3D triangle, project P₀, P₁, P₂ onto it, then build the linear
// map from the resulting 2D coordinates to the UV coordinates. The
// SVD of that map gives the singular values we want.
//
// We compute this on the fly during SLIM iteration rather than storing
// it per-triangle (memory tradeoff: ~50 bytes/tri).
struct TriFrame {
    float E[2][2];  // 2x2 reference frame transform (3D-local → UV)
    // ... (filled in by setupTriFrame, runs once per chart at start)
};

// -----------------------------------------------------------------------------
// Chart extraction
// -----------------------------------------------------------------------------
//
// xatlas's output uvs1 may use any region of [0,1], but charts are
// implicitly defined by UV connectivity: triangles that share a vertex
// AND that vertex has the same uv1 in both triangles → same chart.
// Triangles separated by a chart seam will have different uv1 at the
// shared 3D vertex (because xatlas split that vertex).
//
// Simpler approach: charts are vertex-based connected components in the
// post-xatlas indexed mesh. Union-find by edges of the index buffer.

struct Chart {
    std::vector<uint32_t> triIndices;       // indices into mesh.indices (×3)
    std::vector<uint32_t> vertIndicesLocal; // unique verts referenced
    // Map: global mesh vertex index → 0..chartVertCount-1
    std::unordered_map<uint32_t, uint32_t> globalToLocal;
};

std::vector<Chart> extractCharts(const MeshData& mesh,
                                  int minTriCount) {
    // TODO(next-task):
    //   - Run union-find over vertex indices using the post-xatlas index
    //     buffer. Each connected component is a chart.
    //   - Build per-chart triangle and vertex lists.
    //   - Filter charts with < minTriCount tris (too small to refine).
    //   - Return the chart list.
    (void)mesh; (void)minTriCount;
    return {};
}

// -----------------------------------------------------------------------------
// Projection-aware weight (our novel contribution)
// -----------------------------------------------------------------------------

inline float projectionAwareWeight(const glm::vec3& faceNormal,
                                    const glm::vec3& viewDir,
                                    float exponent, float floorVal) {
    // viewDir points FROM camera TOWARDS scene. We want high weight where
    // the surface faces the camera (i.e. -viewDir aligns with faceNormal).
    float cosTheta = glm::dot(faceNormal, -viewDir);
    float vis      = std::max(cosTheta, 0.0f);
    vis            = std::pow(vis, exponent);
    return std::max(vis, floorVal);
}

// -----------------------------------------------------------------------------
// SLIM core (per-chart)
// -----------------------------------------------------------------------------

double runSlimOnChart(const MeshData& mesh,
                       const Chart& chart,
                       std::vector<glm::vec2>& outUVs,
                       const UvOptimizeOptions& opts)
{
    // ============================================================
    // SLIM iteration (Rabinovich et al. 2017, §4)
    // ============================================================
    //
    // Variables:
    //   u ∈ R^{2N}   stacked UV vector (N = chart vertex count)
    //   T            list of triangles (each ∈ chart)
    //   For each triangle t:
    //     Q_t ∈ R^{2x2}    closest-isometry target Jacobian (set per iter)
    //     w_t ∈ R          reweighting coefficient (scalar from SymDir energy)
    //     M_t ∈ R^{6x4}    local "deformation gradient → flat triangle UV"
    //                       mapping. Constant per triangle, precomputed.
    //
    // Per iteration:
    //   1) Compute current Jacobian J_t at every triangle (from u).
    //   2) SVD: J_t = U_t Σ_t V_tᵀ.
    //   3) Compute SymDir energy density and reweighting weight w_t:
    //         e_t = σ₁² + σ₂² + σ₁⁻² + σ₂⁻²
    //         w_t = sqrt(area_t * vis_t * (e_t / σ_combined))
    //      (see paper for exact derivation; this is the proximal weight)
    //   4) Target Jacobian Q_t = U_t · I · V_tᵀ (closest isometry).
    //   5) Assemble sparse system A · Δu = b where:
    //         A = Σ_t  w_t² · L_tᵀ L_t            (per-triangle Laplacian)
    //         b = Σ_t  w_t² · L_tᵀ flat(Q_t)
    //      L_t is a 4x(2N) selector + linear map that picks the 6 UV
    //      coords of triangle t and produces a 2x2 Jacobian.
    //
    //   6) Solve with Eigen::SimplicialLDLT:
    //         decompose A once per iteration (it changes — w_t depends on u)
    //         backsubstitute to get Δu
    //
    //   7) Line search: α = 1; while any triangle flips at u + α·Δu,
    //      halve α. Bounded by ~16 halvings (≥ 1e-5 step). The flip
    //      check is signed-area test in 2D.
    //
    //   8) Accept u ← u + α · Δu.
    //
    //   9) Check convergence: (E_prev − E_curr) / E_curr < tol → break.
    //
    // After the loop, write u back into outUVs at the chart's vertex
    // positions (using chart.globalToLocal inverse).
    //
    // Implementation notes:
    //   - Pre-compute per-triangle area_t, vis_t, M_t before iter 0.
    //   - A is symmetric positive semi-definite; LDLT is the right
    //     factorisation. Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>>.
    //   - Boundary anchoring: append rows to A with weight = opts.boundary-
    //     AnchorWeight that constrain boundary verts to their initial UV.
    //     This keeps xatlas's packing valid (no chart expansion).
    //   - Use double precision inside the solver; convert to float for the
    //     output. Numerically the solver needs the extra precision around
    //     near-degenerate triangles.

    (void)mesh; (void)chart; (void)outUVs; (void)opts;
    return 0.0;  // TODO: return final energy
}

} // namespace

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------

UvOptimizeResult optimiseUVs(const MeshData& mesh,
                              const UvOptimizeOptions& opts,
                              OptimizeProgressFn cb,
                              void* user)
{
    UvOptimizeResult res;
    auto t0 = std::chrono::steady_clock::now();

    if (mesh.uvs1.empty() || mesh.uvs1.size() != mesh.positions.size()) {
        res.error = "uvs1 missing or size mismatch — run xatlas first";
        return res;
    }

    // 1. Decompose into charts.
    auto charts = extractCharts(mesh, opts.minChartTriangleCount);
    if (cb) cb(5, "Extracted charts", user);

    // 2. Initialise output UVs as a copy of input.
    res.refinedUVs = mesh.uvs1;

    // 3. Per-chart parallel SLIM. Bounded by maxWorkerThreads.
    int nThreads = opts.maxWorkerThreads > 0
        ? opts.maxWorkerThreads
        : std::max(1u, std::thread::hardware_concurrency());

    std::atomic<int>    done {0};
    std::atomic<double> totalE0 {0.0};
    std::atomic<double> totalE1 {0.0};

    // TODO: a thread pool. For now, sketch with std::async batches.
    for (size_t i = 0; i < charts.size(); i += nThreads) {
        std::vector<std::future<void>> fut;
        for (int k = 0; k < nThreads && (i + k) < charts.size(); ++k) {
            const Chart& c = charts[i + k];
            if ((int)c.triIndices.size() < opts.minChartTriangleCount) {
                ++res.chartsSkipped;
                continue;
            }
            fut.emplace_back(std::async(std::launch::async, [&]() {
                double e = runSlimOnChart(mesh, c, res.refinedUVs, opts);
                totalE1.fetch_add(e);
                done.fetch_add(1);
            }));
        }
        for (auto& f : fut) f.wait();
        if (cb) {
            int pct = 10 + 85 * (int)done.load() / (int)charts.size();
            cb(pct, "Refining charts", user);
        }
    }

    auto t1 = std::chrono::steady_clock::now();
    res.elapsedSeconds = std::chrono::duration<double>(t1 - t0).count();
    res.chartsOptimised = (int)done.load();
    res.initialEnergy   = totalE0.load();
    res.finalEnergy     = totalE1.load();
    res.ok              = (res.chartsOptimised > 0);
    if (cb) cb(100, "Done", user);
    return res;
}

} // namespace spacegen
