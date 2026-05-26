#include "MeshDecimate.h"

#include <meshoptimizer.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

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

double estimateAtlasTimeSeconds(int triangleCount) {
    // Two anchor points observed in production on M1 Max:
    //   - 0.5M tris → ~30 s
    //   - 7.7M tris → ~9000 s (extrapolated 51% at 4500s)
    // Fitted exponent: log(9000/30) / log(7.7/0.5) ≈ 2.08
    // We add a 20% margin for SLIM refinement + cache write + hot-reload.
    const double N = std::max(triangleCount, 1) / 1.0e6;
    if (N < 0.01) return 5.0;                  // floor on absurdly small meshes
    const double xatlasSec = 30.0 * std::pow(N / 0.5, 2.08);
    return xatlasSec * 1.20;
}

} // namespace spacegen
