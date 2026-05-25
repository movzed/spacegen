// =============================================================================
// UvAtlasGeogram — alternative atlas backend.
// =============================================================================
//
// When SPACEGEN_HAVE_GEOGRAM is defined (via -DSPACEGEN_ENABLE_GEOGRAM=ON
// at cmake time), this file links against Bruno Lévy's geogram library
// and produces UVs with the same MeshData shape as UvAtlas's generateAtlas.
// When not defined, it returns a not-implemented error so callers can
// gracefully fall back to xatlas.
//
// References:
//   - geogram repo: https://github.com/BrunoLevy/geogram (BSD-3).
//     Lévy, B. — Spectral LSCM (SGP 2008), LSCM (SIGGRAPH 2002).
//   - Function: GEO::mesh_make_atlas() in geogram/mesh/mesh_param.h.

#include "UvAtlasGeogram.h"

#include <cstdio>
#include <cstring>

#ifdef SPACEGEN_HAVE_GEOGRAM
// Geogram public headers. The exact subset varies across versions; this
// targets v1.9.x (the FetchContent tag in CMakeLists.txt).
#include <geogram/basic/common.h>
#include <geogram/basic/logger.h>
#include <geogram/mesh/mesh.h>
#include <geogram/mesh/mesh_io.h>
#include <geogram/mesh/mesh_param.h>
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
// internally whether the runtime has been initialised.
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

    // ---- 1. Build a GEO::Mesh from the input. ----
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
    M.facets.create_triangles(static_cast<GEO::index_t>(nTri));
    for (size_t t = 0; t < nTri; ++t) {
        M.facets.set_vertex(GEO::index_t(t), 0, mesh.indices[t*3 + 0]);
        M.facets.set_vertex(GEO::index_t(t), 1, mesh.indices[t*3 + 1]);
        M.facets.set_vertex(GEO::index_t(t), 2, mesh.indices[t*3 + 2]);
    }
    M.facets.connect();

    if (cb) cb(20, "Running mesh_make_atlas (Spectral LSCM + Tetris)", user);

    // ---- 2. Run geogram's atlas generator. ----
    // hard_angles_threshold maps to Tier-1 Sharp Edges: edges with
    // dihedral > threshold are treated as forced chart boundaries.
    // We pass UvAtlasOptions::sharpEdgeAngleDeg to keep the controls
    // unified across backends.
    double hard_angle = opts.preCutSharpEdges
        ? static_cast<double>(opts.sharpEdgeAngleDeg)
        : 360.0;   // effectively no hard cuts beyond geogram's own heuristics
    try {
        // Newer geogram exposes mesh_make_atlas with multiple options;
        // we use the conservative signature that's stable since v1.7.x.
        GEO::mesh_make_atlas(M, hard_angle);
    } catch (const std::exception& e) {
        res.error = std::string("geogram atlas failed: ") + e.what();
        return res;
    }

    if (cb) cb(80, "Translating geogram output", user);

    // ---- 3. Pull the UV attribute geogram wrote into facet_corners. ----
    // The attribute name is "tex_coord" by geogram convention.
    if (!M.facet_corners.attributes().is_defined("tex_coord")) {
        res.error = "geogram produced no tex_coord attribute";
        return res;
    }
    GEO::Attribute<double> tc(M.facet_corners.attributes(), "tex_coord");
    if (tc.dimension() < 2) {
        res.error = "tex_coord attribute has unexpected dimension";
        return res;
    }

    // geogram writes per-CORNER (per face-vertex) UVs, mirroring xatlas's
    // behaviour — vertices on chart seams are duplicated. We rebuild a
    // MeshData with corner-unique vertices: each face-corner becomes its
    // own output vertex, sharing position/normal with the original input
    // vertex via the corner-to-vertex map.
    const GEO::index_t nF = M.facets.nb();
    res.positions.reserve(nF * 3);
    res.normals.reserve(nF * 3);
    res.uvs0.reserve(nF * 3);
    res.uvs1.reserve(nF * 3);
    res.indices.reserve(nF * 3);

    const bool haveN  = !mesh.normals.empty() && mesh.normals.size() == mesh.positions.size();
    const bool haveU0 = !mesh.uvs.empty()      && mesh.uvs.size()    == mesh.positions.size();

    // Deduplicate corners with identical (origVertex, uv). Keeps memory
    // tight without re-stitching seams.
    struct CornerKey {
        uint32_t origV; float u, v;
        bool operator==(const CornerKey& o) const {
            return origV == o.origV && u == o.u && v == o.v;
        }
    };
    struct CornerKeyHash {
        size_t operator()(const CornerKey& k) const {
            uint64_t h = uint64_t(k.origV) * 0x9E3779B185EBCA87ULL;
            h ^= std::hash<float>{}(k.u);
            h ^= std::hash<float>{}(k.v);
            return size_t(h);
        }
    };
    std::unordered_map<CornerKey, uint32_t, CornerKeyHash> dedup;
    dedup.reserve(nF * 3 / 2);

    for (GEO::index_t f = 0; f < nF; ++f) {
        GEO::index_t c0 = M.facets.corners_begin(f);
        for (GEO::index_t k = 0; k < 3; ++k) {
            GEO::index_t c = c0 + k;
            GEO::index_t v = M.facet_corners.vertex(c);
            CornerKey key { v, (float)tc[c*2 + 0], (float)tc[c*2 + 1] };
            auto it = dedup.find(key);
            uint32_t newIdx;
            if (it == dedup.end()) {
                newIdx = static_cast<uint32_t>(res.positions.size());
                dedup.emplace(key, newIdx);
                res.positions.push_back(mesh.positions[v]);
                res.normals.push_back(haveN ? mesh.normals[v]
                                              : glm::vec3(0,0,1));
                res.uvs0.push_back(haveU0 ? mesh.uvs[v] : glm::vec2(0.0f));
                res.uvs1.push_back(glm::vec2(key.u, key.v));
            } else {
                newIdx = it->second;
            }
            res.indices.push_back(newIdx);
        }
    }

    if (cb) cb(95, "Building output mesh", user);

    res.chartCount  = 0;          // geogram doesn't surface chart count directly
    res.atlasWidth  = 0;
    res.atlasHeight = 0;
    res.ok          = true;

    std::printf("[UvAtlasGeogram] mesh_make_atlas done: in=%zu verts → "
                 "out=%zu verts, %zu indices\n",
                 mesh.positions.size(), res.positions.size(),
                 res.indices.size());

    if (cb) cb(100, "Done", user);
    return res;
}

#endif // SPACEGEN_HAVE_GEOGRAM

} // namespace spacegen
