// =============================================================================
// UvAtlasGeogram — alternative atlas backend (Tier 4).
// =============================================================================
//
// When SPACEGEN_HAVE_GEOGRAM is defined (via -DSPACEGEN_ENABLE_GEOGRAM=ON
// at cmake time), this file links against Bruno Lévy's geogram library
// and produces UVs with the same MeshData shape as UvAtlas's generateAtlas.
// When not defined, it returns a not-implemented error so callers can
// gracefully fall back to xatlas.
//
// Pipeline (matches the "Spectral LSCM + Tetris packing" the Tier-4
// README promises):
//   1. Build a GEO::Mesh from MeshData positions/indices.
//   2. Repair/orient it so geogram sees a clean manifold (LSCM needs
//      topological disks; degenerate input trips geo_assert()).
//   3. GEO::mesh_make_atlas(M, hardAngle, PARAM_SPECTRAL_LSCM, PACK_TETRIS)
//      - segments the mesh into charts (sharp-edge + VSA segmentation),
//      - flattens each chart with Spectral Conformal Parameterization
//        (Mullen-Tong-Alliez-Desbrun 2008); geogram falls back to plain
//        LSCM (Lévy 2002) automatically if the ARPACK OpenNL extension
//        is not present, so this is always safe to request,
//      - packs the charts with the built-in "Tetris" packer (Ray, in the
//        original LSCM article) and normalizes UVs into [0,1]².
//   4. Read the per-facet-corner "tex_coord" attribute back, dedupe
//      corners, and emit a MeshData-shaped result identical in layout to
//      the xatlas path (vertices split at chart seams).
//
// References:
//   - geogram repo: https://github.com/BrunoLevy/geogram (BSD-3), v1.9.1.
//     Lévy, Petitjean, Ray, Maillot — LSCM (SIGGRAPH 2002).
//     Mullen, Tong, Alliez, Desbrun — Spectral Conformal Param. (SGP 2008).
//   - API: GEO::mesh_make_atlas() / GEO::mesh_get_charts() in
//     geogram/parameterization/mesh_atlas_maker.h.

#include "UvAtlasGeogram.h"

#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef SPACEGEN_HAVE_GEOGRAM
// Geogram public headers (v1.9.1 — the FetchContent tag in CMakeLists.txt).
// NOTE: the parameterization API lives under geogram/parameterization/, NOT
// the (nonexistent in 1.9.x) geogram/mesh/mesh_param.h.
#include <geogram/basic/common.h>
#include <geogram/basic/logger.h>
#include <geogram/basic/attributes.h>
#include <geogram/mesh/mesh.h>
#include <geogram/mesh/mesh_repair.h>
#include <geogram/parameterization/mesh_atlas_maker.h>
#endif

namespace spacegen {

bool geogramAvailable() {
#ifdef SPACEGEN_HAVE_GEOGRAM
    return true;
#else
    return false;
#endif
}

#ifndef SPACEGEN_HAVE_GEOGRAM
// Stub used when the engine was built without geogram.
UvAtlasResult generateAtlasGeogram(const MeshData&,
                                    const UvAtlasOptions&,
                                    ProgressFn, void*)
{
    UvAtlasResult r;
    r.ok    = false;
    r.error = "Geogram not compiled in. Rebuild with "
              "-DSPACEGEN_ENABLE_GEOGRAM=ON to enable the Tier 4 backend.";
    return r;
}
#else

namespace {

// One-time geogram bootstrap. Idempotent across calls — geogram tracks
// internally whether the runtime has been initialised, but we still guard
// with a function-local static so we only touch the Logger once.
struct GeogramBootstrap {
    GeogramBootstrap() {
        GEO::initialize(GEO::GEOGRAM_INSTALL_NONE);
        GEO::Logger::instance()->set_quiet(true);
    }
};

void ensureGeogramInit() {
    static GeogramBootstrap b;
    (void)b;
}

} // namespace

UvAtlasResult generateAtlasGeogram(const MeshData& mesh,
                                    const UvAtlasOptions& opts,
                                    ProgressFn cb,
                                    void* user)
{
    UvAtlasResult res;
    if (mesh.positions.empty() || mesh.indices.empty()) {
        res.error = "Empty mesh (no positions or indices)";
        return res;
    }
    if (mesh.indices.size() % 3 != 0) {
        res.error = "Index count is not a multiple of 3";
        return res;
    }
    ensureGeogramInit();

    if (cb) cb(2, "Building geogram mesh", user);

    // ---- 1. Build a GEO::Mesh from the input. ----------------------------
    // Vertices are created 1:1 with mesh.positions and in the same order, so
    // a geogram vertex index equals the original input vertex index. We rely
    // on that below to copy positions/normals/uv0 straight from the input.
    GEO::Mesh M;
    M.vertices.set_dimension(3);
    M.vertices.create_vertices(static_cast<GEO::index_t>(mesh.positions.size()));
    for (size_t i = 0; i < mesh.positions.size(); ++i) {
        double* p = M.vertices.point_ptr(GEO::index_t(i));
        p[0] = mesh.positions[i].x;
        p[1] = mesh.positions[i].y;
        p[2] = mesh.positions[i].z;
    }
    const size_t nTri = mesh.indices.size() / 3;
    GEO::index_t firstTri = M.facets.create_triangles(
        static_cast<GEO::index_t>(nTri));
    for (size_t t = 0; t < nTri; ++t) {
        GEO::index_t f = firstTri + static_cast<GEO::index_t>(t);
        M.facets.set_vertex(f, 0, mesh.indices[t * 3 + 0]);
        M.facets.set_vertex(f, 1, mesh.indices[t * 3 + 1]);
        M.facets.set_vertex(f, 2, mesh.indices[t * 3 + 2]);
    }
    M.facets.connect();

    if (cb) cb(10, "Repairing mesh topology", user);

    // ---- 1b. Clean the mesh so geogram's parameterizer is happy. ----------
    // LSCM/ABF rely on consistent facet orientation + a valid half-edge
    // structure and assert on incoherent winding. We deliberately do NOT
    // request MESH_REPAIR_COLOCATE here: colocation renumbers/merges vertices,
    // which would break the "geogram vertex index == input vertex index"
    // identity we use below to copy positions/normals/uv0 straight from the
    // input. Topology dissociation (always done) + duplicate-facet removal
    // keep vertices 1:1 with the input; mesh_reorient() then makes the
    // per-chart border walk well-defined. Both are no-ops on clean input, so
    // good meshes are not perturbed.
    GEO::mesh_repair(
        M,
        GEO::MeshRepairMode(GEO::MESH_REPAIR_TOPOLOGY | GEO::MESH_REPAIR_DUP_F),
        /*colocate_epsilon=*/0.0);
    GEO::mesh_reorient(M);

    if (cb) cb(20, "mesh_make_atlas (Spectral LSCM + Tetris)", user);

    // ---- 2. Run geogram's atlas generator. --------------------------------
    // hard_angles_threshold maps to the Tier-1 "Sharp Edges" control: edges
    // whose dihedral angle exceeds the threshold are forced chart boundaries.
    // We pass UvAtlasOptions::sharpEdgeAngleDeg to keep the controls unified
    // across backends. When pre-cut is disabled we pass 180° so only
    // geogram's own segmentation heuristics introduce seams.
    const double hardAngle = opts.preCutSharpEdges
        ? static_cast<double>(opts.sharpEdgeAngleDeg)
        : 180.0;
    try {
        // Spectral LSCM for the per-chart flattening (degrades to plain LSCM
        // if ARPACK is unavailable) + the built-in Tetris packer, which also
        // normalizes the resulting UVs into [0,1]² (see mesh_param_packer.cpp
        // Packer::pack_surface).
        GEO::mesh_make_atlas(
            M,
            hardAngle,
            GEO::PARAM_SPECTRAL_LSCM,
            GEO::PACK_TETRIS,
            /*verbose=*/false);
    } catch (const std::exception& e) {
        res.error = std::string("geogram mesh_make_atlas failed: ") + e.what();
        return res;
    } catch (...) {
        res.error = "geogram mesh_make_atlas failed (unknown exception)";
        return res;
    }

    if (cb) cb(80, "Translating geogram output", user);

    // ---- 3. Pull the UV attribute geogram wrote into facet_corners. -------
    // mesh_make_atlas stores per-CORNER (per face-vertex) UVs in a dimension-2
    // double vector attribute named "tex_coord" on facet_corners — vertices
    // on chart seams therefore carry different UVs per incident face, exactly
    // like xatlas's vertex split. The values are already normalized to [0,1]
    // by the Tetris packer, so (unlike the xatlas path) we do NOT divide by an
    // atlas pixel size.
    if (!GEO::Attribute<double>::is_defined(
            M.facet_corners.attributes(), "tex_coord", 2)) {
        res.error = "geogram produced no dimension-2 tex_coord attribute "
                    "(parameterization likely failed)";
        return res;
    }
    GEO::Attribute<double> tc(M.facet_corners.attributes(), "tex_coord");

    // Number of UV charts/islands, recomputed from the committed tex coords.
    const int chartCount = static_cast<int>(GEO::mesh_get_charts(M));

    // We rebuild a MeshData with corner-unique vertices: each face-corner
    // becomes its own output vertex, sharing position/normal/uv0 with the
    // original input vertex via the (geogram vertex == input vertex) identity.
    const GEO::index_t nF = M.facets.nb();
    res.positions.reserve(size_t(nF) * 3);
    res.normals.reserve(size_t(nF) * 3);
    res.uvs0.reserve(size_t(nF) * 3);
    res.uvs1.reserve(size_t(nF) * 3);
    res.indices.reserve(size_t(nF) * 3);

    const bool haveN  = !mesh.normals.empty()
                        && mesh.normals.size() == mesh.positions.size();
    const bool haveU0 = !mesh.uvs.empty()
                        && mesh.uvs.size() == mesh.positions.size();
    const uint32_t origVertCount = static_cast<uint32_t>(mesh.positions.size());

    // Deduplicate corners with identical (origVertex, uv): keeps the output
    // vertex count tight without re-stitching seams (matches xatlas, which
    // shares a vertex when position + uv coincide across faces).
    struct CornerKey {
        uint32_t origV; float u, v;
        bool operator==(const CornerKey& o) const {
            return origV == o.origV && u == o.u && v == o.v;
        }
    };
    struct CornerKeyHash {
        size_t operator()(const CornerKey& k) const {
            uint64_t h = uint64_t(k.origV) * 0x9E3779B185EBCA87ULL;
            h ^= std::hash<float>{}(k.u) * 0x100000001B3ULL;
            h ^= std::hash<float>{}(k.v);
            return size_t(h);
        }
    };
    std::unordered_map<CornerKey, uint32_t, CornerKeyHash> dedup;
    dedup.reserve(size_t(nF) * 3 / 2 + 1);

    for (GEO::index_t f = 0; f < nF; ++f) {
        const GEO::index_t c0 = M.facets.corners_begin(f);
        for (GEO::index_t k = 0; k < 3; ++k) {
            const GEO::index_t c = c0 + k;
            GEO::index_t v = M.facet_corners.vertex(c);
            // After mesh_repair the geogram vertex index still indexes the
            // (colocated) input vertices; clamp defensively just in case.
            uint32_t origV = (v < origVertCount) ? uint32_t(v) : 0u;

            CornerKey key { origV,
                            float(tc[size_t(c) * 2 + 0]),
                            float(tc[size_t(c) * 2 + 1]) };
            auto it = dedup.find(key);
            uint32_t newIdx;
            if (it == dedup.end()) {
                newIdx = static_cast<uint32_t>(res.positions.size());
                dedup.emplace(key, newIdx);
                res.positions.push_back(mesh.positions[origV]);
                res.normals.push_back(haveN ? mesh.normals[origV]
                                            : glm::vec3(0, 0, 1));
                res.uvs0.push_back(haveU0 ? mesh.uvs[origV] : glm::vec2(0.0f));
                res.uvs1.push_back(glm::vec2(key.u, key.v));
            } else {
                newIdx = it->second;
            }
            res.indices.push_back(newIdx);
        }
    }

    if (cb) cb(95, "Building output mesh", user);

    // Atlas dimensions: the Tetris packer emits UVs already normalized to
    // [0,1], so there is no intrinsic pixel size. Report a nominal square
    // resolution (the operator's requested max, or the packer's 1024 default)
    // so the cache header + UI have sane non-zero dimensions. uvs1 themselves
    // are resolution-independent — the texel size is chosen at sample time.
    const int nominal = opts.maxAtlasSize > 0 ? opts.maxAtlasSize : 1024;
    res.chartCount  = chartCount;
    res.atlasWidth  = nominal;
    res.atlasHeight = nominal;
    res.ok          = true;

    std::printf("[UvAtlasGeogram] Spectral LSCM + Tetris done: in=%zu verts -> "
                "out=%zu verts (%+d), %zu indices, %d charts, atlas %dx%d\n",
                mesh.positions.size(), res.positions.size(),
                int(res.positions.size()) - int(mesh.positions.size()),
                res.indices.size(), res.chartCount,
                res.atlasWidth, res.atlasHeight);

    if (cb) cb(100, "Done", user);
    return res;
}

#endif // SPACEGEN_HAVE_GEOGRAM

} // namespace spacegen
