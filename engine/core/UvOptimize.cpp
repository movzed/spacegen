// =============================================================================
// SpaceGen Optimize — implementation
// =============================================================================
//
// SLIM-based projection-aware UV optimizer. See engine/core/UvOptimize.h for
// the API. The algorithm follows the spec at
//   ~/.claude/skills/uv-mastery/references/spacegen-optimize-spec.md
// which is verbatim Section 9 of the canonical uv-mastery reference.
//
// Original sources cited:
//   Rabinovich, Poranne, Panozzo, Sorkine-Hornung, "Scalable Locally Injective
//   Mappings", ACM TOG (SIGGRAPH 2017).  https://igl.ethz.ch/projects/slim/
//
// SpaceGen's original twist on top of SLIM is the projection-aware weight
// w(N) = saturate(dot(N, -projector_dir))^k applied per-triangle. It biases
// optimisation budget towards faces the audience sees frontally and lets
// grazing facets accept more stretch. Not present in any prior published
// algorithm we found (2026).

#include "UvOptimize.h"

#include <Eigen/Dense>
#include <Eigen/Sparse>
#include <Eigen/SparseCholesky>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <future>
#include <thread>
#include <unordered_map>
#include <vector>

namespace spacegen {

namespace {

constexpr double EPS = 1e-12;

// Per-chart data passed between extraction and SLIM refinement.
// Private to UvOptimize.cpp — public API works at the mesh level.
struct UvOptimizeChart {
    Eigen::MatrixXd  V3D;          // n×3 chart vertex positions (3D)
    Eigen::MatrixXi  F;            // m×3 triangle indices (chart-local)
    Eigen::MatrixXd  UV;           // n×2 input/output UVs (modified in place)
    std::vector<int> boundary;     // chart-local indices of fixed verts
};

// -----------------------------------------------------------------------------
// Per-triangle precomputed data
// -----------------------------------------------------------------------------
struct TriPrecomp {
    int            v[3];      // chart-local vertex indices (0..N-1)
    double         area3d;    // 3D triangle area
    double         weight;    // projection-aware × area
    Eigen::Matrix2d Q_inv;    // inverse of 2D rest frame [e1 e2]^T mapped from edges
    glm::vec3      normal;    // face normal, for debug / introspection
};

// 3D area of a triangle.
inline double triArea3D(const Eigen::Vector3d& a,
                         const Eigen::Vector3d& b,
                         const Eigen::Vector3d& c) {
    return 0.5 * (b - a).cross(c - a).norm();
}

// -----------------------------------------------------------------------------
// Build a local 2D frame on a 3D triangle and express its 3 vertices in it.
// Returns the 2x2 matrix [q1 q2] where q0 = (0,0) is implicit at the origin.
// Q_inv is the inverse of [q1-q0, q2-q0] used to derive the Jacobian.
// -----------------------------------------------------------------------------
Eigen::Matrix2d computeRestQInv(const Eigen::Vector3d& p0,
                                 const Eigen::Vector3d& p1,
                                 const Eigen::Vector3d& p2)
{
    // Local frame: e1 along p1-p0, e2 perpendicular in the triangle plane.
    Eigen::Vector3d e1 = p1 - p0;
    double len1 = e1.norm();
    if (len1 < EPS) return Eigen::Matrix2d::Identity();
    e1 /= len1;
    Eigen::Vector3d n = e1.cross(p2 - p0);
    double area_x2 = n.norm();
    if (area_x2 < EPS) return Eigen::Matrix2d::Identity();
    n /= area_x2;
    Eigen::Vector3d e2 = n.cross(e1);
    // Project p1-p0 and p2-p0 into (e1, e2).
    Eigen::Matrix2d Q;
    Q(0, 0) = len1;                                // (p1-p0) . e1
    Q(1, 0) = 0.0;                                  // (p1-p0) . e2 (by construction)
    Q(0, 1) = (p2 - p0).dot(e1);
    Q(1, 1) = (p2 - p0).dot(e2);
    if (std::abs(Q.determinant()) < EPS) return Eigen::Matrix2d::Identity();
    return Q.inverse();
}

// -----------------------------------------------------------------------------
// Symmetric Dirichlet energy and SLIM proxy weight (spec section 9.5)
// -----------------------------------------------------------------------------
inline double symmetricDirichletDensity(double s1, double s2) {
    s1 = std::max(s1, EPS);
    s2 = std::max(s2, EPS);
    return s1*s1 + s2*s2 + 1.0/(s1*s1) + 1.0/(s2*s2);
}

inline double slimProxyWeight(double s) {
    // f(s) = s^2 + 1/s^2; f'(s) = 2s - 2/s^3.
    // SLIM modified-Hessian weight per singular value:
    //   w(s) = f'(s) / (2*(s - 1))  when s != 1, else lim->1 = f''(1)/2 = 4.
    if (std::abs(s - 1.0) < 1e-6) return 4.0;
    double num = 2.0 * s - 2.0 / (s*s*s);
    double w = num / (2.0 * (s - 1.0));
    // Numerical safety: keep positive and bounded.
    return std::max(w, 1e-6);
}

// std::atomic<double>::fetch_add only landed in C++20. Implement the
// equivalent via compare_exchange so this builds on the C++17 baseline.
inline void atomicAddDouble(std::atomic<double>& dst, double value) {
    double cur = dst.load(std::memory_order_relaxed);
    while (!dst.compare_exchange_weak(cur, cur + value,
                                       std::memory_order_relaxed)) {
        // cur is auto-refreshed by compare_exchange_weak on failure.
    }
}

// SVD of a 2x2 matrix → (U, S, V) with U, V proper rotations.
// Handles the reflection case so R = U V^T is a rotation, not a reflection.
struct Svd2x2 {
    Eigen::Matrix2d U, V;
    double s1, s2;
};

Svd2x2 svd2x2(const Eigen::Matrix2d& J) {
    Eigen::JacobiSVD<Eigen::Matrix2d> svd(J, Eigen::ComputeFullU | Eigen::ComputeFullV);
    Eigen::Matrix2d U = svd.matrixU();
    Eigen::Matrix2d V = svd.matrixV();
    Eigen::Vector2d S = svd.singularValues();
    double s1 = S(0), s2 = S(1);
    // Ensure right-handed UV so closest-isometry target is a rotation, not flip.
    if ((U * V.transpose()).determinant() < 0) {
        U.col(1) *= -1.0;
        s2 = -s2;
    }
    return { U, V, s1, s2 };
}

// -----------------------------------------------------------------------------
// Build per-triangle precomputed data for one chart.
// -----------------------------------------------------------------------------
std::vector<TriPrecomp> precomputeTris(const UvOptimizeChart& chart,
                                        const UvOptimizeOptions& opts,
                                        double& totalArea)
{
    std::vector<TriPrecomp> tris;
    const int nT = (int)chart.F.rows();
    tris.reserve(nT);
    totalArea = 0.0;
    glm::vec3 view = opts.viewDir;
    if (glm::length(view) > 0.0f) view = glm::normalize(view);
    for (int t = 0; t < nT; ++t) {
        int i0 = chart.F(t, 0), i1 = chart.F(t, 1), i2 = chart.F(t, 2);
        Eigen::Vector3d p0 = chart.V3D.row(i0);
        Eigen::Vector3d p1 = chart.V3D.row(i1);
        Eigen::Vector3d p2 = chart.V3D.row(i2);
        TriPrecomp p;
        p.v[0] = i0; p.v[1] = i1; p.v[2] = i2;
        p.area3d = triArea3D(p0, p1, p2);
        if (p.area3d < EPS) continue;        // skip degenerate
        p.Q_inv = computeRestQInv(p0, p1, p2);
        // Projection-aware weight (our novel addition).
        Eigen::Vector3d ne = (p1 - p0).cross(p2 - p0);
        ne.normalize();
        glm::vec3 N(ne.x(), ne.y(), ne.z());
        double vis = 1.0;
        if (opts.projectionAware) {
            float cosT = std::max(glm::dot(N, -view), 0.0f);
            vis = std::pow((double)cosT, (double)opts.visExponent);
            vis = std::max(vis, (double)opts.visFloor);
        }
        p.weight = p.area3d * vis;
        p.normal = N;
        totalArea += p.weight;
        tris.push_back(p);
    }
    return tris;
}

// -----------------------------------------------------------------------------
// Compute per-triangle Jacobian from current UVs.
// J = [u1-u0, u2-u0] * Q_inv   (2x2).
// -----------------------------------------------------------------------------
inline Eigen::Matrix2d computeJacobian(const Eigen::MatrixXd& UV,
                                        const TriPrecomp& t)
{
    Eigen::Matrix2d E;
    E.col(0) = (UV.row(t.v[1]) - UV.row(t.v[0])).transpose();
    E.col(1) = (UV.row(t.v[2]) - UV.row(t.v[0])).transpose();
    return E * t.Q_inv;
}

// Total energy across all tris.
double computeEnergy(const Eigen::MatrixXd& UV,
                     const std::vector<TriPrecomp>& tris)
{
    double E = 0.0;
    for (const auto& t : tris) {
        Eigen::Matrix2d J = computeJacobian(UV, t);
        Eigen::JacobiSVD<Eigen::Matrix2d> svd(J);
        Eigen::Vector2d S = svd.singularValues();
        E += t.weight * symmetricDirichletDensity(S(0), S(1));
    }
    return E;
}

// Find the maximum α ∈ [0, 1] along (UV → UV + α·dUV) such that no triangle
// flips (signed area sign preserved). Returns 1.0 if no flip occurs at α=1.
double maxStepNoFlip(const Eigen::MatrixXd& UV,
                     const Eigen::MatrixXd& dUV,
                     const std::vector<TriPrecomp>& tris)
{
    double alpha_min = 1.0;
    for (const auto& t : tris) {
        Eigen::Vector2d u0 = UV.row(t.v[0]).transpose();
        Eigen::Vector2d u1 = UV.row(t.v[1]).transpose();
        Eigen::Vector2d u2 = UV.row(t.v[2]).transpose();
        Eigen::Vector2d d0 = dUV.row(t.v[0]).transpose();
        Eigen::Vector2d d1 = dUV.row(t.v[1]).transpose();
        Eigen::Vector2d d2 = dUV.row(t.v[2]).transpose();
        // Signed area A(α) is quadratic in α:
        //   A(α) = a + b·α + c·α²
        // where for the three vertices p_i(α) = u_i + α·d_i,
        // A = ½ [(p1.x - p0.x)(p2.y - p0.y) - (p1.y - p0.y)(p2.x - p0.x)].
        // We want A(α) > 0 (same sign as initial area, which we assume is > 0
        // after the xatlas seed).
        auto eval = [&](double a) {
            Eigen::Vector2d P0 = u0 + a * d0;
            Eigen::Vector2d P1 = u1 + a * d1;
            Eigen::Vector2d P2 = u2 + a * d2;
            return 0.5 * ((P1.x()-P0.x())*(P2.y()-P0.y())
                        - (P1.y()-P0.y())*(P2.x()-P0.x()));
        };
        double A0 = eval(0.0);
        double A1 = eval(1.0);
        if (A0 > 0 && A1 > 0) continue;          // safe across full step
        if (A0 <= 0) { alpha_min = 0.0; break; } // already flipped (shouldn't)
        // Binary search for the boundary where A(α) crosses 0.
        double lo = 0.0, hi = 1.0;
        for (int it = 0; it < 30; ++it) {
            double mid = 0.5 * (lo + hi);
            if (eval(mid) > 0) lo = mid; else hi = mid;
        }
        alpha_min = std::min(alpha_min, lo);
    }
    return alpha_min;
}

} // namespace

// =============================================================================
// Public API
// =============================================================================

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
    if (mesh.indices.size() % 3 != 0) {
        res.error = "non-triangle mesh (indices not a multiple of 3)";
        return res;
    }

    // ---- 1. Chart extraction via union-find on the index buffer.
    //         Vertices connected by any triangle are in the same chart.
    if (cb) cb(2, "Extracting charts", user);
    const size_t nV = mesh.positions.size();
    std::vector<int> parent(nV);
    for (size_t i = 0; i < nV; ++i) parent[i] = (int)i;
    auto find = [&](int x){ while (parent[x]!=x){ parent[x]=parent[parent[x]]; x=parent[x]; } return x; };
    auto unite = [&](int a, int b){ int ra=find(a), rb=find(b); if (ra!=rb) parent[ra]=rb; };
    const size_t nT = mesh.indices.size() / 3;
    for (size_t t = 0; t < nT; ++t) {
        uint32_t a = mesh.indices[t*3+0], b = mesh.indices[t*3+1], c = mesh.indices[t*3+2];
        unite(a, b); unite(a, c);
    }
    // Group triangles by root.
    std::unordered_map<int, std::vector<uint32_t>> chartTris;
    for (size_t t = 0; t < nT; ++t) {
        int r = find(mesh.indices[t*3+0]);
        chartTris[r].push_back((uint32_t)t);
    }

    // ---- 2. Build UvOptimizeChart instances per chart.
    if (cb) cb(8, "Preparing per-chart data", user);
    std::vector<UvOptimizeChart> charts;
    charts.reserve(chartTris.size());
    // Maps: chart-local vertex → global vertex (per chart).
    std::vector<std::vector<uint32_t>> chartLocal2Global;
    chartLocal2Global.reserve(chartTris.size());
    int skipped = 0;
    for (auto& kv : chartTris) {
        if ((int)kv.second.size() < opts.minChartTriangleCount) {
            ++skipped;
            continue;
        }
        UvOptimizeChart ch;
        std::unordered_map<uint32_t, int> g2l;
        std::vector<uint32_t> l2g;
        // Collect verts referenced by this chart's tris.
        for (uint32_t tIdx : kv.second) {
            for (int k = 0; k < 3; ++k) {
                uint32_t v = mesh.indices[tIdx*3 + k];
                if (g2l.find(v) == g2l.end()) {
                    g2l[v] = (int)l2g.size();
                    l2g.push_back(v);
                }
            }
        }
        const int N = (int)l2g.size();
        const int M = (int)kv.second.size();
        ch.V3D.resize(N, 3);
        ch.UV.resize(N, 2);
        ch.F.resize(M, 3);
        for (int i = 0; i < N; ++i) {
            const glm::vec3& p = mesh.positions[l2g[i]];
            const glm::vec2& u = mesh.uvs1[l2g[i]];
            ch.V3D.row(i) << p.x, p.y, p.z;
            ch.UV.row(i)  << u.x, u.y;
        }
        for (int i = 0; i < M; ++i) {
            uint32_t tIdx = kv.second[i];
            ch.F(i, 0) = g2l[mesh.indices[tIdx*3+0]];
            ch.F(i, 1) = g2l[mesh.indices[tIdx*3+1]];
            ch.F(i, 2) = g2l[mesh.indices[tIdx*3+2]];
        }
        // Boundary = all verts on the chart's outer ring (preserves xatlas packing).
        // A vertex is boundary if any of its incident triangles has an edge
        // that's not shared with another triangle in THIS chart.
        std::unordered_map<uint64_t, int> edgeCount;
        edgeCount.reserve(M * 3);
        auto edgeKey = [](int a, int b) -> uint64_t {
            int lo = std::min(a, b), hi = std::max(a, b);
            return (uint64_t(lo) << 32) | uint64_t(hi);
        };
        for (int i = 0; i < M; ++i) {
            int a = ch.F(i,0), b = ch.F(i,1), c = ch.F(i,2);
            ++edgeCount[edgeKey(a,b)];
            ++edgeCount[edgeKey(b,c)];
            ++edgeCount[edgeKey(c,a)];
        }
        std::vector<bool> isBoundary(N, false);
        for (auto& ec : edgeCount) {
            if (ec.second != 1) continue;        // interior edge (shared)
            int lo = (int)(ec.first >> 32);
            int hi = (int)(ec.first & 0xFFFFFFFFu);
            isBoundary[lo] = true;
            isBoundary[hi] = true;
        }
        for (int i = 0; i < N; ++i) if (isBoundary[i]) ch.boundary.push_back(i);
        charts.push_back(std::move(ch));
        chartLocal2Global.push_back(std::move(l2g));
    }
    res.chartsSkipped = skipped;
    if (charts.empty()) {
        res.error = "no charts large enough to optimise";
        return res;
    }

    // Hard cap on chart count. SLIM allocates Eigen LDLT workspace per
    // chart; 200k+ tiny charts on an over-cut mesh have OOM'd a 32 GB box.
    // Sort charts by triangle count descending and refine the top N. The
    // remaining (small) charts keep their xatlas output, which is already
    // decent on near-planar geometry.
    if ((int)charts.size() > opts.maxChartsRefined) {
        std::sort(charts.begin(), charts.end(),
            [](const UvOptimizeChart& a, const UvOptimizeChart& b) {
                return a.F.rows() > b.F.rows();
            });
        int dropped = (int)charts.size() - opts.maxChartsRefined;
        charts.resize(opts.maxChartsRefined);
        res.chartsSkipped += dropped;
        std::fprintf(stderr,
            "[UvOptimize] Chart count %d exceeds cap %d — refining top %d "
            "by triangle count, leaving %d charts at xatlas-only quality.\n",
            (int)charts.size() + dropped, opts.maxChartsRefined,
            opts.maxChartsRefined, dropped);
    }

    std::fprintf(stderr, "[UvOptimize] SLIM starting on %zu charts "
                          "(min=%d tris, parallel=%d threads)...\n",
                          charts.size(), opts.minChartTriangleCount,
                          (opts.maxWorkerThreads > 0
                            ? opts.maxWorkerThreads
                            : (int)std::max(1u, std::thread::hardware_concurrency() - 1)));

    // ---- 3. Per-chart SLIM, parallel.
    if (cb) cb(15, "Refining charts (SLIM iter)", user);
    int nThreads = opts.maxWorkerThreads > 0
        ? opts.maxWorkerThreads
        : (int)std::max(1u, std::thread::hardware_concurrency() - 1);

    std::atomic<int>    progress {0};
    std::atomic<double> sumInitial {0.0};
    std::atomic<double> sumFinal   {0.0};
    std::atomic<int>    chartsDone {0};
    std::atomic<bool>   cancelled  {false};

    // Refine one chart (runs on a worker thread).
    auto refineOne = [&](UvOptimizeChart& chart) -> double {
        double area = 0.0;
        auto tris = precomputeTris(chart, opts, area);
        if (tris.empty()) return 0.0;
        const int N = (int)chart.V3D.rows();

        // Boundary mask.
        std::vector<bool> fixed(N, false);
        for (int b : chart.boundary) fixed[b] = true;
        // Index map: chart-local vertex → interior-only index (or -1).
        std::vector<int> intIdx(N, -1);
        int nInt = 0;
        for (int i = 0; i < N; ++i) if (!fixed[i]) intIdx[i] = nInt++;
        if (nInt == 0) return 0.0;

        Eigen::MatrixXd UV = chart.UV;          // working copy (double)
        double E_init = computeEnergy(UV, tris);
        atomicAddDouble(sumInitial, E_init);
        double E_prev = E_init;

        // Build the topology of the sparse system once. The per-triangle
        // 3x3 block of M^T M (where M is the 2x3 selector that turns UVs
        // into the Jacobian column) is independent of the weights — but
        // we DO need to refactor each iter because the per-tri weight
        // changes. So we keep a triplet list and rebuild A every iter.
        typedef Eigen::SparseMatrix<double> SpMat;
        typedef Eigen::Triplet<double>      Trip;
        const int dim = nInt;

        for (int iter = 0; iter < opts.maxIterations; ++iter) {
            if (cancelled.load()) break;

            // ---- Local step: per-triangle SVD and weight ----
            std::vector<double> tw1(tris.size()), tw2(tris.size());
            std::vector<Eigen::Matrix2d> tR(tris.size());
            const double epsSV = (double)opts.epsSingularValue;
            for (size_t i = 0; i < tris.size(); ++i) {
                Eigen::Matrix2d J = computeJacobian(UV, tris[i]);
                Svd2x2 s = svd2x2(J);
                tR[i] = s.U * s.V.transpose();
                tw1[i] = slimProxyWeight(std::max(std::abs(s.s1), epsSV));
                tw2[i] = slimProxyWeight(std::max(std::abs(s.s2), epsSV));
            }

            // ---- Global step: build (A | rhs_x, rhs_y) and solve ----
            std::vector<Trip> trips;
            trips.reserve(tris.size() * 9);
            Eigen::VectorXd bx = Eigen::VectorXd::Zero(dim);
            Eigen::VectorXd by = Eigen::VectorXd::Zero(dim);

            for (size_t ti = 0; ti < tris.size(); ++ti) {
                const TriPrecomp& t = tris[ti];
                const Eigen::Matrix2d& H = t.Q_inv;
                // Per-tri 2x3 selector M: maps (u0, u1, u2) (each a scalar
                // for x or y) → (J0, J1) (two scalars per row of J).
                //   M[0] = [ -(H00+H10),  H00,  H10 ]
                //   M[1] = [ -(H01+H11),  H01,  H11 ]
                double m00 = -(H(0,0) + H(1,0)), m01 = H(0,0), m02 = H(1,0);
                double m10 = -(H(0,1) + H(1,1)), m11 = H(0,1), m12 = H(1,1);
                // Per-tri scalar weight (combined SLIM proxy + projection vis).
                double s = t.weight * 0.5 * (tw1[ti] + tw2[ti]);

                // 3×3 quadratic contribution = s · M^T M (3×3), where M is 2×3.
                double a00 = m00*m00 + m10*m10;
                double a01 = m00*m01 + m10*m11;
                double a02 = m00*m02 + m10*m12;
                double a11 = m01*m01 + m11*m11;
                double a12 = m01*m02 + m11*m12;
                double a22 = m02*m02 + m12*m12;

                // RHS = s · M^T · R (per row of R for x and y respectively).
                double rx0 = tR[ti](0, 0), rx1 = tR[ti](0, 1);
                double ry0 = tR[ti](1, 0), ry1 = tR[ti](1, 1);
                double mxe0 = m00*rx0 + m10*rx1;
                double mxe1 = m01*rx0 + m11*rx1;
                double mxe2 = m02*rx0 + m12*rx1;
                double mye0 = m00*ry0 + m10*ry1;
                double mye1 = m01*ry0 + m11*ry1;
                double mye2 = m02*ry0 + m12*ry1;

                // Spread the 3 (vertex×vertex) entries into the sparse triplet
                // list, scaling by s. For fixed (boundary) verts, move their
                // contribution to the RHS as Dirichlet conditions instead.
                int idx[3] = { t.v[0], t.v[1], t.v[2] };
                double A33[3][3] = {
                    { a00, a01, a02 },
                    { a01, a11, a12 },
                    { a02, a12, a22 } };
                double bx_local[3] = { mxe0, mxe1, mxe2 };
                double by_local[3] = { mye0, mye1, mye2 };
                for (int r = 0; r < 3; ++r) {
                    int gi = idx[r];
                    int ii = intIdx[gi];
                    if (ii < 0) continue;            // boundary row: skip
                    for (int c = 0; c < 3; ++c) {
                        int gj = idx[c];
                        int jj = intIdx[gj];
                        double v = s * A33[r][c];
                        if (jj >= 0) {
                            trips.emplace_back(ii, jj, v);
                        } else {
                            // boundary contribution → move to RHS
                            bx(ii) -= v * UV(gj, 0);
                            by(ii) -= v * UV(gj, 1);
                        }
                    }
                    bx(ii) += s * bx_local[r];
                    by(ii) += s * by_local[r];
                }
            }

            // Assemble.
            SpMat A(dim, dim);
            A.setFromTriplets(trips.begin(), trips.end());
            // Ridge term for numerical stability on near-singular charts.
            for (int i = 0; i < dim; ++i) trips.emplace_back(i, i, 1e-9);
            A.setFromTriplets(trips.begin(), trips.end());

            // Solve A · x = bx, A · y = by.
            Eigen::SimplicialLDLT<SpMat> solver;
            solver.compute(A);
            if (solver.info() != Eigen::Success) break;
            Eigen::VectorXd xNew = solver.solve(bx);
            Eigen::VectorXd yNew = solver.solve(by);
            if (solver.info() != Eigen::Success) break;

            // Build candidate UV matrix.
            Eigen::MatrixXd UV_new = UV;
            for (int i = 0; i < N; ++i) {
                int ii = intIdx[i];
                if (ii >= 0) {
                    UV_new(i, 0) = xNew(ii);
                    UV_new(i, 1) = yNew(ii);
                }
            }
            Eigen::MatrixXd dUV = UV_new - UV;

            // Line search for injectivity + energy decrease.
            double max_alpha = opts.flipPreventionEnabled
                ? std::max(0.0, 0.99 * maxStepNoFlip(UV, dUV, tris))
                : 1.0;
            double alpha = std::min(1.0, max_alpha);
            const int  maxBack = 25;
            double E_cand = 0.0;
            int back;
            for (back = 0; back < maxBack; ++back) {
                Eigen::MatrixXd UV_try = UV + alpha * dUV;
                E_cand = computeEnergy(UV_try, tris);
                if (E_cand < E_prev * (1.0 + 1e-8)) break;
                alpha *= 0.5;
                if (alpha < 1e-12) break;
            }
            if (back == maxBack || alpha < 1e-12) break;

            UV += alpha * dUV;
            double rel = (E_prev - E_cand) / std::max(E_prev, EPS);
            E_prev = E_cand;
            if (rel < opts.convergenceTol) break;
        }

        // Write result back into chart.UV (which the caller's data structure
        // will read into the global uvs1 array).
        chart.UV = UV;
        atomicAddDouble(sumFinal, E_prev);
        return E_prev;
    };

    // Submit work in chunks.
    size_t i0 = 0;
    while (i0 < charts.size() && !cancelled.load()) {
        std::vector<std::future<void>> futures;
        size_t end = std::min(charts.size(), i0 + (size_t)nThreads);
        for (size_t i = i0; i < end; ++i) {
            futures.emplace_back(std::async(std::launch::async, [&, i](){
                refineOne(charts[i]);
                chartsDone.fetch_add(1);
            }));
        }
        for (auto& f : futures) f.wait();
        i0 = end;
        if (cb) {
            int pct = 15 + 80 * (int)chartsDone.load() / (int)charts.size();
            if (!cb(pct, "Refining charts (SLIM)", user)) {
                cancelled.store(true);
                break;
            }
        }
    }

    // ---- 4. Stitch refined chart UVs back into the global uvs1 array.
    if (cb) cb(97, "Writing results", user);
    res.refinedUVs = mesh.uvs1;
    for (size_t ci = 0; ci < charts.size(); ++ci) {
        const auto& ch = charts[ci];
        const auto& l2g = chartLocal2Global[ci];
        for (int i = 0; i < (int)l2g.size(); ++i) {
            res.refinedUVs[l2g[i]] = glm::vec2(
                (float)ch.UV(i, 0), (float)ch.UV(i, 1));
        }
    }

    auto t1 = std::chrono::steady_clock::now();
    res.elapsedSeconds = std::chrono::duration<double>(t1 - t0).count();
    res.chartsOptimised = (int)chartsDone.load();
    res.initialEnergy   = sumInitial.load();
    res.finalEnergy     = sumFinal.load();
    res.ok              = !cancelled.load() && res.chartsOptimised > 0;
    if (cancelled.load()) res.error = "cancelled by operator";
    if (cb) cb(100, "Done", user);

    std::printf("[UvOptimize] SLIM: %d charts refined (+%d skipped), "
                 "energy %.2f → %.2f (%.1f%% reduction) in %.1fs\n",
                 res.chartsOptimised, res.chartsSkipped,
                 res.initialEnergy, res.finalEnergy,
                 res.initialEnergy > 0
                    ? 100.0 * (1.0 - res.finalEnergy / res.initialEnergy)
                    : 0.0,
                 res.elapsedSeconds);
    return res;
}

} // namespace spacegen
