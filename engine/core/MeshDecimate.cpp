#include "MeshDecimate.h"

#include <meshoptimizer.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

namespace spacegen {

DecimateResult decimateMesh(MeshData& mesh, float ratio) {
    DecimateResult r;
    if (mesh.positions.empty() || mesh.indices.empty()) {
        r.error = "Empty mesh (no positions or indices)";
        return r;
    }
    if (mesh.indices.size() % 3 != 0) {
        r.error = "Index count is not a multiple of 3";
        return r;
    }
    r.trianglesBefore = static_cast<int>(mesh.indices.size() / 3);
    if (ratio >= 0.999f) {
        // No-op short-circuit; saves a few seconds on full-density runs.
        r.trianglesAfter = r.trianglesBefore;
        r.ok = true;
        return r;
    }
    if (ratio <= 0.0f) {
        r.error = "Ratio must be > 0";
        return r;
    }

    auto t0 = std::chrono::steady_clock::now();

    const size_t indexCount  = mesh.indices.size();
    const size_t vertexCount = mesh.positions.size();
    size_t targetIndexCount =
        static_cast<size_t>(static_cast<double>(indexCount) * ratio);
    targetIndexCount -= targetIndexCount % 3;
    if (targetIndexCount < 3) targetIndexCount = 3;

    std::vector<uint32_t> newIndices(indexCount);
    float lodError = 0.0f;
    size_t newCount = 0;

    // Prefer the attributes-aware variant so UV-dense regions (the mask
    // unwrap) are protected from over-aggressive collapse. Falls back to
    // the basic variant when uvs are unavailable.
    const bool haveUv = !mesh.uvs.empty() && mesh.uvs.size() == vertexCount;
    if (haveUv) {
        // Pack a (uv.x, uv.y) attribute buffer matching positions stride.
        std::vector<float> attrs(vertexCount * 2);
        for (size_t i = 0; i < vertexCount; ++i) {
            attrs[i * 2 + 0] = mesh.uvs[i].x;
            attrs[i * 2 + 1] = mesh.uvs[i].y;
        }
        const float attrWeights[2] = { 0.5f, 0.5f };
        newCount = meshopt_simplifyWithAttributes(
            newIndices.data(),
            mesh.indices.data(),
            indexCount,
            reinterpret_cast<const float*>(mesh.positions.data()),
            vertexCount,
            sizeof(glm::vec3),
            attrs.data(),
            sizeof(float) * 2,
            attrWeights,
            2,
            targetIndexCount,
            /*target_error=*/1.0f,
            /*options=*/meshopt_SimplifyLockBorder,
            &lodError);
    } else {
        newCount = meshopt_simplify(
            newIndices.data(),
            mesh.indices.data(),
            indexCount,
            reinterpret_cast<const float*>(mesh.positions.data()),
            vertexCount,
            sizeof(glm::vec3),
            targetIndexCount,
            /*target_error=*/1.0f,
            /*options=*/meshopt_SimplifyLockBorder,
            &lodError);
    }
    newIndices.resize(newCount);
    mesh.indices = std::move(newIndices);

    // Any previously-loaded atlas (uvs1) is now stale — it referenced
    // the old index buffer. Drop it; the auto-hybrid shader will mirror
    // UV0 until the operator regenerates with Generate.
    mesh.uvs1.clear();

    auto t1 = std::chrono::steady_clock::now();
    r.elapsedSec = std::chrono::duration<double>(t1 - t0).count();
    r.trianglesAfter = static_cast<int>(mesh.indices.size() / 3);
    r.lodError = lodError;
    r.ok = true;

    std::printf("[MeshDecimate] %d → %d tris (%.1f%% kept, lodError %.3f) in %.2fs\n",
                 r.trianglesBefore, r.trianglesAfter,
                 100.0 * r.trianglesAfter / std::max(1, r.trianglesBefore),
                 r.lodError, r.elapsedSec);
    return r;
}

AtlasTimeRange estimateAtlasTimeRange(int triangleCount) {
    // Two power-law fits bracketing observed runs:
    //
    //   Optimistic  : t = 30 × (N/0.5)^1.5  — clean mesh
    //                                         t(0.5M)= 30s
    //                                         t(2M)  = 4.0 min
    //                                         t(7.7M)= 30 min
    //
    //   Pessimistic : t = 30 × (N/0.5)^3.95 — degenerate-heavy mesh
    //                                         t(0.5M)= 30s
    //                                         t(2M)  = 2 h  (observed)
    //                                         t(7.7M)≫ 1 day (unbounded;
    //                                                          capped below)
    //
    // The +20% margin on each side covers SLIM refinement + cache write
    // + hot-reload. The pessimistic curve is capped at 6 h so the panel
    // doesn't show "ETA: 14 days" on truly absurd inputs — at that
    // point the user should decimate, not wait.
    const double N = std::max(triangleCount, 1) / 1.0e6;
    AtlasTimeRange r;
    if (N < 0.01) { r.lowSec = r.highSec = 5.0; return r; }
    double opt  = 30.0 * std::pow(N / 0.5, 1.5);
    double pess = 30.0 * std::pow(N / 0.5, 3.95);
    pess = std::min(pess, 6.0 * 3600.0);   // cap at 6 h
    r.lowSec  = opt  * 1.20;
    r.highSec = pess * 1.20;
    return r;
}

// ============================================================
// UV-bake proxy pipeline
// ============================================================

// Weld vertices that share a 3D position. Blender's glTF exporter splits
// vertices at every UV / normal / material seam, leaving the proxy
// topologically disconnected: two faces that share an edge in 3D but were
// authored across a seam have separate vertex indices.
//
// xatlas walks the index buffer to build its chart graph — disconnected
// vertices → disconnected charts → fragmentation explosion. Welding by
// position only (ignoring uv / normal) restores the connectivity that
// matches the actual 3D geometry. We do NOT touch the dense mesh, so the
// artist's UV0 unwrap on the render mesh is preserved.
//
// Uses meshopt_generateVertexRemap with a position-only buffer: it
// produces a remap from (old vertex index) → (welded vertex index), and
// we apply that remap to the index buffer while compacting the position
// array. Returns the number of vertices removed.
static int weldByPosition(MeshData& mesh) {
    if (mesh.positions.empty() || mesh.indices.empty()) return 0;
    const size_t vCount = mesh.positions.size();
    std::vector<uint32_t> remap(vCount);
    meshopt_Stream posStream { mesh.positions.data(), sizeof(glm::vec3),
                                sizeof(glm::vec3) };
    size_t newVCount = meshopt_generateVertexRemapMulti(
        remap.data(),
        mesh.indices.data(),
        mesh.indices.size(),
        vCount,
        &posStream, 1);
    if (newVCount == vCount) return 0;     // already welded

    // Apply remap to the index buffer.
    std::vector<uint32_t> newIndices(mesh.indices.size());
    meshopt_remapIndexBuffer(newIndices.data(),
                              mesh.indices.data(),
                              mesh.indices.size(),
                              remap.data());
    mesh.indices = std::move(newIndices);

    // Compact positions; drop normals/uvs (proxy doesn't need them for
    // xatlas, and they'd require per-attribute remapping with potentially
    // conflicting values at welded points).
    std::vector<glm::vec3> newPositions(newVCount);
    meshopt_remapVertexBuffer(newPositions.data(),
                               mesh.positions.data(),
                               vCount, sizeof(glm::vec3),
                               remap.data());
    mesh.positions = std::move(newPositions);
    mesh.normals.clear();
    mesh.uvs.clear();
    mesh.uvs1.clear();
    return static_cast<int>(vCount - newVCount);
}

// Strip triangles that are degenerate in 3D: any pair of vertex positions
// equal within `eps`. These come from Blender exports where seams in
// UV/normal space create distinct vertex indices at the SAME 3D point —
// downstream tools (xatlas, our Sharp-Edges pre-cut) see them as either
// zero-length edges or zero-area faces and inflate the chart count.
static int dropDegenerateTriangles(MeshData& mesh, float eps = 1e-7f) {
    if (mesh.indices.size() % 3 != 0 || mesh.positions.empty()) return 0;
    const auto& P = mesh.positions;
    std::vector<uint32_t> kept;
    kept.reserve(mesh.indices.size());
    int dropped = 0;
    const size_t nT = mesh.indices.size() / 3;
    for (size_t t = 0; t < nT; ++t) {
        uint32_t i = mesh.indices[t*3+0], j = mesh.indices[t*3+1], k = mesh.indices[t*3+2];
        if (i == j || j == k || i == k) { ++dropped; continue; }
        const glm::vec3& a = P[i]; const glm::vec3& b = P[j]; const glm::vec3& c = P[k];
        // Zero-area test: cross product magnitude ≈ 0.
        glm::vec3 e0 = b - a, e1 = c - a;
        float areaSq = glm::dot(glm::cross(e0, e1), glm::cross(e0, e1));
        if (areaSq < eps) { ++dropped; continue; }
        kept.push_back(i); kept.push_back(j); kept.push_back(k);
    }
    mesh.indices = std::move(kept);
    return dropped;
}

MeshData buildUvBakeProxy(const MeshData& dense, float ratio) {
    MeshData proxy;
    proxy.name        = dense.name + "_uv_proxy";
    proxy.transform   = dense.transform;
    proxy.materialIdx = dense.materialIdx;
    proxy.positions   = dense.positions;
    proxy.normals     = dense.normals;
    proxy.uvs         = dense.uvs;
    proxy.uvs1        = {};                  // populated by xatlas
    proxy.indices     = dense.indices;       // start from full topology, then decimate

    // Pre-clean: strip degenerate triangles BEFORE decimation. The
    // Blender export carries thousands of zero-area faces from UV/normal
    // seam splits; without this xatlas inflates the chart count by 10×
    // because every degenerate face looks like a hard edge to it.
    int dropped0 = dropDegenerateTriangles(proxy);
    if (dropped0 > 0) {
        std::printf("[MeshDecimate] proxy pre-clean: dropped %d degenerate "
                     "triangles from dense before decimation\n", dropped0);
    }

    if (ratio < 0.999f) {
        DecimateResult dr = decimateMesh(proxy, ratio);
        if (!dr.ok) {
            std::fprintf(stderr,
                "[MeshDecimate] proxy build failed: %s — returning unwhittled copy\n",
                dr.error.c_str());
        }
    }

    // Post-clean: meshoptimizer occasionally emits new degenerate
    // triangles on a noisy input. Belt-and-braces.
    int dropped1 = dropDegenerateTriangles(proxy);
    if (dropped1 > 0) {
        std::printf("[MeshDecimate] proxy post-clean: dropped %d degenerate "
                     "triangles after decimation\n", dropped1);
    }
    // Weld split vertices: without this, xatlas sees the proxy as
    // disconnected per seam and fragments into 100k+ tiny charts.
    int welded = weldByPosition(proxy);
    if (welded > 0) {
        std::printf("[MeshDecimate] proxy weld: merged %d duplicate-position "
                     "vertices (xatlas connectivity restored)\n", welded);
    }
    return proxy;
}

// ----- Closest point on triangle (Ericson, RTCD §5.1.5) --------------------

namespace {

struct Bary { float a, b, c; };

inline float closestPointOnTriangle(
    const glm::vec3& p,
    const glm::vec3& a, const glm::vec3& b, const glm::vec3& c,
    Bary& outBary)
{
    const glm::vec3 ab = b - a, ac = c - a, ap = p - a;
    const float d1 = glm::dot(ab, ap);
    const float d2 = glm::dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) {
        outBary = {1.0f, 0.0f, 0.0f}; return glm::dot(ap, ap);
    }
    const glm::vec3 bp = p - b;
    const float d3 = glm::dot(ab, bp);
    const float d4 = glm::dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) {
        outBary = {0.0f, 1.0f, 0.0f}; return glm::dot(bp, bp);
    }
    const glm::vec3 cp = p - c;
    const float d5 = glm::dot(ab, cp);
    const float d6 = glm::dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) {
        outBary = {0.0f, 0.0f, 1.0f}; return glm::dot(cp, cp);
    }
    const float vc = d1*d4 - d3*d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        const float v = d1 / (d1 - d3);
        outBary = {1.0f - v, v, 0.0f};
        glm::vec3 pt = a + v * ab;
        return glm::dot(p - pt, p - pt);
    }
    const float vb = d5*d2 - d1*d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        const float w = d2 / (d2 - d6);
        outBary = {1.0f - w, 0.0f, w};
        glm::vec3 pt = a + w * ac;
        return glm::dot(p - pt, p - pt);
    }
    const float va = d3*d6 - d5*d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        outBary = {0.0f, 1.0f - w, w};
        glm::vec3 pt = b + w * (c - b);
        return glm::dot(p - pt, p - pt);
    }
    const float denom = 1.0f / (va + vb + vc);
    const float v = vb * denom;
    const float w = vc * denom;
    outBary = {1.0f - v - w, v, w};
    glm::vec3 pt = a + v * ab + w * ac;
    return glm::dot(p - pt, p - pt);
}

// ----- Uniform spatial grid over proxy triangles ----------------------------

struct TriGrid {
    glm::vec3   bmin {0.0f}, bmax{0.0f};
    glm::vec3   invCell{0.0f};
    glm::ivec3  dim {1, 1, 1};
    std::vector<std::vector<uint32_t>> cells;
    inline int cellIndex(int x, int y, int z) const {
        return ((z * dim.y) + y) * dim.x + x;
    }
    inline glm::ivec3 clampCoord(const glm::vec3& p) const {
        glm::vec3 r = (p - bmin) * invCell;
        return glm::ivec3(
            std::min(dim.x - 1, std::max(0, (int)std::floor(r.x))),
            std::min(dim.y - 1, std::max(0, (int)std::floor(r.y))),
            std::min(dim.z - 1, std::max(0, (int)std::floor(r.z))));
    }
};

TriGrid buildTriGrid(const std::vector<glm::vec3>& positions,
                      const std::vector<uint32_t>& indices)
{
    TriGrid g;
    if (positions.empty() || indices.empty()) return g;

    glm::vec3 mn( std::numeric_limits<float>::max());
    glm::vec3 mx(-std::numeric_limits<float>::max());
    for (const auto& p : positions) { mn = glm::min(mn, p); mx = glm::max(mx, p); }
    g.bmin = mn; g.bmax = mx;
    glm::vec3 diag = mx - mn;
    float diagLen = glm::length(diag);

    // Target ~64³ cells, capped per-axis at 128.
    float targetCellSize = diagLen / 64.0f;
    if (targetCellSize <= 0.0f) targetCellSize = 1.0f;
    g.dim.x = std::min(128, std::max(1, (int)std::ceil(diag.x / targetCellSize)));
    g.dim.y = std::min(128, std::max(1, (int)std::ceil(diag.y / targetCellSize)));
    g.dim.z = std::min(128, std::max(1, (int)std::ceil(diag.z / targetCellSize)));
    glm::vec3 cellSz = glm::vec3(
        diag.x / g.dim.x, diag.y / g.dim.y, diag.z / g.dim.z);
    cellSz = glm::max(cellSz, glm::vec3(1e-6f));
    g.invCell = 1.0f / cellSz;

    g.cells.assign(static_cast<size_t>(g.dim.x) * g.dim.y * g.dim.z, {});
    const size_t nTris = indices.size() / 3;
    for (uint32_t t = 0; t < nTris; ++t) {
        const glm::vec3& A = positions[indices[t*3+0]];
        const glm::vec3& B = positions[indices[t*3+1]];
        const glm::vec3& C = positions[indices[t*3+2]];
        glm::vec3 tmn = glm::min(glm::min(A, B), C);
        glm::vec3 tmx = glm::max(glm::max(A, B), C);
        glm::ivec3 lo = g.clampCoord(tmn);
        glm::ivec3 hi = g.clampCoord(tmx);
        for (int z = lo.z; z <= hi.z; ++z)
        for (int y = lo.y; y <= hi.y; ++y)
        for (int x = lo.x; x <= hi.x; ++x) {
            g.cells[g.cellIndex(x, y, z)].push_back(t);
        }
    }
    return g;
}

} // namespace

TransferResult transferUv1FromProxyToDense(
    const MeshData& dense,
    const MeshData& proxy,
    std::vector<glm::vec2>& denseOutUv1,
    TransferProgressFn cb,
    void* user)
{
    TransferResult r;
    if (dense.positions.empty() || proxy.positions.empty() || proxy.indices.empty()) {
        r.error = "Empty input (dense.positions or proxy.positions/indices)";
        return r;
    }
    if (proxy.uvs1.size() != proxy.positions.size()) {
        r.error = "proxy.uvs1.size() != proxy.positions.size()";
        return r;
    }
    auto t0 = std::chrono::steady_clock::now();

    // Build spatial grid over proxy triangles.
    TriGrid grid = buildTriGrid(proxy.positions, proxy.indices);

    denseOutUv1.assign(dense.positions.size(), glm::vec2(0.0f));
    const size_t nVerts = dense.positions.size();
    const int progressEvery = static_cast<int>(std::max<size_t>(1, nVerts / 100));
    int progressTick = 0;

    for (size_t i = 0; i < nVerts; ++i) {
        const glm::vec3& p = dense.positions[i];
        glm::ivec3 c0 = grid.clampCoord(p);

        // Spiral outward in 3×3×3 → 5×5×5 if needed.
        float bestDistSq = std::numeric_limits<float>::max();
        Bary  bestBary {1, 0, 0};
        uint32_t bestTri = 0;

        auto scanCell = [&](int x, int y, int z) {
            if (x < 0 || y < 0 || z < 0
                || x >= grid.dim.x || y >= grid.dim.y || z >= grid.dim.z) return;
            const auto& tris = grid.cells[grid.cellIndex(x, y, z)];
            for (uint32_t t : tris) {
                const glm::vec3& A = proxy.positions[proxy.indices[t*3+0]];
                const glm::vec3& B = proxy.positions[proxy.indices[t*3+1]];
                const glm::vec3& C = proxy.positions[proxy.indices[t*3+2]];
                Bary bary;
                float d2 = closestPointOnTriangle(p, A, B, C, bary);
                if (d2 < bestDistSq) {
                    bestDistSq = d2;
                    bestBary   = bary;
                    bestTri    = t;
                }
            }
        };

        // First pass: 3×3×3 around the dense vertex's cell.
        for (int dz = -1; dz <= 1; ++dz)
        for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx)
            scanCell(c0.x + dx, c0.y + dy, c0.z + dz);

        // Fallback: widen to 5×5×5 if no triangle found.
        if (bestDistSq == std::numeric_limits<float>::max()) {
            for (int dz = -2; dz <= 2; ++dz)
            for (int dy = -2; dy <= 2; ++dy)
            for (int dx = -2; dx <= 2; ++dx) {
                if (std::abs(dx) < 2 && std::abs(dy) < 2 && std::abs(dz) < 2) continue;
                scanCell(c0.x + dx, c0.y + dy, c0.z + dz);
            }
        }
        // Last resort: full bbox scan. Should be rare.
        if (bestDistSq == std::numeric_limits<float>::max()) {
            for (int z = 0; z < grid.dim.z; ++z)
            for (int y = 0; y < grid.dim.y; ++y)
            for (int x = 0; x < grid.dim.x; ++x)
                scanCell(x, y, z);
        }

        const glm::vec2& uvA = proxy.uvs1[proxy.indices[bestTri*3+0]];
        const glm::vec2& uvB = proxy.uvs1[proxy.indices[bestTri*3+1]];
        const glm::vec2& uvC = proxy.uvs1[proxy.indices[bestTri*3+2]];
        denseOutUv1[i] = uvA * bestBary.a + uvB * bestBary.b + uvC * bestBary.c;

        if (cb && ++progressTick >= progressEvery) {
            progressTick = 0;
            int pct = static_cast<int>(100 * i / nVerts);
            if (!cb(pct, user)) {
                r.error = "Cancelled";
                return r;
            }
        }
    }

    r.denseVerts  = static_cast<int>(nVerts);
    r.proxyTris   = static_cast<int>(proxy.indices.size() / 3);
    auto t1 = std::chrono::steady_clock::now();
    r.elapsedSec  = std::chrono::duration<double>(t1 - t0).count();
    r.ok = true;
    if (cb) cb(100, user);

    std::printf("[MeshDecimate] UV1 transfer: %d dense verts ← %d proxy tris in %.2fs\n",
                 r.denseVerts, r.proxyTris, r.elapsedSec);
    return r;
}

void bakeCameraProjectedUv1(const MeshData& dense,
                             const glm::mat4& projection,
                             const glm::mat4& view,
                             std::vector<glm::vec2>& outUv1)
{
    const size_t N = dense.positions.size();
    outUv1.assign(N, glm::vec2(-1.0f));
    if (N == 0) return;

    const glm::mat4 vp = projection * view * dense.transform;
    int outside = 0;
    for (size_t i = 0; i < N; ++i) {
        glm::vec4 clip = vp * glm::vec4(dense.positions[i], 1.0f);
        if (clip.w <= 1e-4f) {
            outUv1[i] = glm::vec2(-1.0f);   // behind / on camera plane
            ++outside;
            continue;
        }
        float u = (clip.x / clip.w) * 0.5f + 0.5f;
        float v = 1.0f - ((clip.y / clip.w) * 0.5f + 0.5f);
        outUv1[i] = glm::vec2(u, v);
    }
    std::printf("[MeshDecimate] camera-projected UV1 bake: %zu verts "
                 "(%d behind camera → uv=(-1,-1))\n",
                 N, outside);
}

double estimateAtlasTimeSeconds(int triangleCount) {
    // Use the PESSIMISTIC end of the range as the single point estimate.
    // Production Blender exports almost always carry degenerate faces /
    // zero-length edges / non-manifold regions, which are what dominate
    // xatlas's chart-computation cost. Optimistic predictions were off
    // by ~6× on the user's reference scene; this matches reality.
    return estimateAtlasTimeRange(triangleCount).highSec;
}

} // namespace spacegen
