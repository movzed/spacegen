#include "UvAtlas.h"

#include <xatlas.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <unordered_map>
#include <vector>

namespace spacegen {

namespace {

// xatlas progress callbacks. xatlas calls the registered ProgressFunc
// with a category + progress (0..100) + user pointer. We forward to the
// ProgressFn signature our wrapper exposes.
struct ProgressBridge {
    ProgressFn fn;
    void*      user;
};

bool xatlasProgressForward(xatlas::ProgressCategory category,
                            int progress, void* userData)
{
    auto* b = static_cast<ProgressBridge*>(userData);
    if (!b || !b->fn) return true;
    const char* name = "?";
    switch (category) {
        case xatlas::ProgressCategory::AddMesh:        name = "Validating geometry"; break;
        case xatlas::ProgressCategory::ComputeCharts:  name = "Detecting charts"; break;
        case xatlas::ProgressCategory::PackCharts:     name = "Packing atlas"; break;
        case xatlas::ProgressCategory::BuildOutputMeshes: name = "Building output"; break;
    }
    // Forward to the user callback and respect its cancellation request.
    // Returning false tells xatlas to abort the current phase ASAP.
    return b->fn(progress, name, b->user);
}

int xatlasPrintForward(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::fprintf(stderr, "[xatlas] ");
    int n = std::vfprintf(stderr, fmt, args);
    va_end(args);
    return n;
}

} // namespace

// ============================================================
// Sharp-edge pre-cut (RizomUV-style)
// ============================================================
//
// Splits the mesh at every manifold edge whose dihedral angle exceeds
// `angleDeg`. The output is topologically identical to the input but has
// vertex duplicates along the sharp edges; consequently, downstream
// chart-detection (xatlas) is FORCED to treat sharp edges as chart
// boundaries because they appear as topological discontinuities.
//
// Algorithm:
//   1. Compute one face normal per triangle.
//   2. Build an edge → adjacent-triangles map (at most 2 tris per
//      manifold edge).
//   3. Build a union-find over (triangle × corner-in-triangle) — i.e.
//      the "face-corner" graph. There are 3*nTris corners total. Two
//      corners at the same vertex are merged iff the edge that connects
//      their triangles across that vertex is NOT sharp.
//   4. For each face-corner, the union-find root identifies the
//      partition. Each partition becomes one new vertex in the output
//      mesh. Attributes (position / normal / uv) are copied from the
//      original vertex referenced by that corner.
//
// Cost: O(corners) time, O(corners) memory. For 7.7M triangles =
// 23M corners → ~92 MB for the union-find parents, manageable.

namespace {

struct EdgeKey {
    uint32_t a, b;   // sorted: a < b
    bool operator==(const EdgeKey& o) const noexcept { return a == o.a && b == o.b; }
};
struct EdgeKeyHash {
    size_t operator()(const EdgeKey& e) const noexcept {
        return std::hash<uint64_t>{}((uint64_t(e.a) << 32) ^ uint64_t(e.b));
    }
};

struct UnionFind {
    std::vector<int32_t> parent;
    explicit UnionFind(size_t n) : parent(n) {
        for (size_t i = 0; i < n; ++i) parent[i] = static_cast<int32_t>(i);
    }
    int32_t find(int32_t x) {
        while (parent[x] != x) {
            parent[x] = parent[parent[x]];   // path compression
            x = parent[x];
        }
        return x;
    }
    void unite(int32_t a, int32_t b) {
        int32_t ra = find(a), rb = find(b);
        if (ra != rb) parent[ra] = rb;
    }
};

} // namespace

MeshData preCutOnSharpEdges(const MeshData& mesh, float angleDeg) {
    if (mesh.indices.size() % 3 != 0 || mesh.positions.empty()) return mesh;
    const size_t nTris = mesh.indices.size() / 3;
    const float  cosThreshold = std::cos(angleDeg * 3.14159265358979f / 180.0f);

    // 1. Face normals
    std::vector<glm::vec3> faceN(nTris);
    for (size_t t = 0; t < nTris; ++t) {
        const glm::vec3& p0 = mesh.positions[mesh.indices[t*3 + 0]];
        const glm::vec3& p1 = mesh.positions[mesh.indices[t*3 + 1]];
        const glm::vec3& p2 = mesh.positions[mesh.indices[t*3 + 2]];
        glm::vec3 n = glm::cross(p1 - p0, p2 - p0);
        float len = glm::length(n);
        faceN[t] = (len > 1e-12f) ? n / len : glm::vec3(0.0f, 0.0f, 1.0f);
    }

    // 2. Edge → adjacent triangles (pair). Non-manifold edges (>2 tris)
    //    or boundary edges (1 tri) are skipped; xatlas already handles
    //    them by virtue of the topology itself.
    struct EdgeAdj { int32_t t0 = -1, t1 = -1; };
    std::unordered_map<EdgeKey, EdgeAdj, EdgeKeyHash> edges;
    edges.reserve(nTris * 3 / 2 + 1);
    for (uint32_t t = 0; t < nTris; ++t) {
        for (int e = 0; e < 3; ++e) {
            uint32_t a = mesh.indices[t*3 + e];
            uint32_t b = mesh.indices[t*3 + (e + 1) % 3];
            EdgeKey k { std::min(a, b), std::max(a, b) };
            auto& adj = edges[k];
            if      (adj.t0 < 0) adj.t0 = static_cast<int32_t>(t);
            else if (adj.t1 < 0) adj.t1 = static_cast<int32_t>(t);
            // else: non-manifold; ignore extra references.
        }
    }

    // 3. Union-find over face-corners: corner id = t*3 + i.
    UnionFind uf(nTris * 3);
    for (const auto& [edge, adj] : edges) {
        if (adj.t0 < 0 || adj.t1 < 0) continue;          // boundary
        float cosA = glm::dot(faceN[adj.t0], faceN[adj.t1]);
        bool  sharp = cosA < cosThreshold;
        if (sharp) continue;                             // keep separated

        // Locate the corners at vertex `edge.a` and `edge.b` in each triangle.
        int32_t cAa = -1, cAb = -1, cBa = -1, cBb = -1;
        for (int k = 0; k < 3; ++k) {
            uint32_t v0 = mesh.indices[adj.t0 * 3 + k];
            uint32_t v1 = mesh.indices[adj.t1 * 3 + k];
            if (v0 == edge.a) cAa = adj.t0 * 3 + k;
            if (v0 == edge.b) cAb = adj.t0 * 3 + k;
            if (v1 == edge.a) cBa = adj.t1 * 3 + k;
            if (v1 == edge.b) cBb = adj.t1 * 3 + k;
        }
        if (cAa >= 0 && cBa >= 0) uf.unite(cAa, cBa);
        if (cAb >= 0 && cBb >= 0) uf.unite(cAb, cBb);
    }

    // 4. Emit one new vertex per union-find component, copy attributes
    //    from the original referenced vertex.
    const bool haveNormals = !mesh.normals.empty()
                              && mesh.normals.size() == mesh.positions.size();
    const bool haveUv0     = !mesh.uvs.empty()
                              && mesh.uvs.size() == mesh.positions.size();
    const bool haveUv1     = !mesh.uvs1.empty()
                              && mesh.uvs1.size() == mesh.positions.size();

    MeshData out;
    out.name        = mesh.name;
    out.transform   = mesh.transform;
    out.materialIdx = mesh.materialIdx;
    out.indices.resize(mesh.indices.size());
    out.positions.reserve(mesh.positions.size() * 11 / 10);  // ~10% growth est.
    if (haveNormals) out.normals.reserve(out.positions.capacity());
    if (haveUv0)     out.uvs.reserve(out.positions.capacity());
    if (haveUv1)     out.uvs1.reserve(out.positions.capacity());

    std::unordered_map<int32_t, uint32_t> rootToNew;
    rootToNew.reserve(mesh.positions.size() * 11 / 10);

    for (size_t c = 0; c < nTris * 3; ++c) {
        int32_t root = uf.find(static_cast<int32_t>(c));
        auto it = rootToNew.find(root);
        uint32_t newIdx;
        if (it == rootToNew.end()) {
            newIdx = static_cast<uint32_t>(out.positions.size());
            rootToNew.emplace(root, newIdx);
            uint32_t orig = mesh.indices[c];
            out.positions.push_back(mesh.positions[orig]);
            if (haveNormals) out.normals.push_back(mesh.normals[orig]);
            if (haveUv0)     out.uvs    .push_back(mesh.uvs[orig]);
            if (haveUv1)     out.uvs1   .push_back(mesh.uvs1[orig]);
        } else {
            newIdx = it->second;
        }
        out.indices[c] = newIdx;
    }

    std::printf("[UvAtlas] Sharp-edge pre-cut (>%.1f°): %zu → %zu verts "
                 "(+%zu, %.1f%%), %zu sharp edges\n",
                 angleDeg, mesh.positions.size(), out.positions.size(),
                 out.positions.size() - mesh.positions.size(),
                 100.0 * (out.positions.size() - mesh.positions.size())
                       / std::max<size_t>(1, mesh.positions.size()),
                 [&]{ size_t s=0; for (auto& e : edges)
                       if (e.second.t0>=0 && e.second.t1>=0
                           && glm::dot(faceN[e.second.t0], faceN[e.second.t1]) < cosThreshold) ++s;
                      return s; }());
    return out;
}

UvAtlasResult generateAtlas(const MeshData& mesh,
                             const UvAtlasOptions& opts,
                             ProgressFn progressCb,
                             void* progressUser)
{
    UvAtlasResult res;

    if (mesh.positions.empty() || mesh.indices.empty()) {
        res.error = "Empty mesh (no positions or indices)";
        return res;
    }
    if (mesh.indices.size() % 3 != 0) {
        res.error = "Index count is not a multiple of 3 (non-triangle mesh)";
        return res;
    }

    ProgressBridge bridge { progressCb, progressUser };

    // verbose=true so xatlas's per-phase progress is visible in stderr too,
    // not just via the structured progress callback. Useful when debugging
    // long runs on dense meshes.
    xatlas::SetPrint(xatlasPrintForward, /*verbose=*/true);
    xatlas::Atlas* atlas = xatlas::Create();
    xatlas::SetProgressCallback(atlas, xatlasProgressForward, &bridge);

    // ---- Describe the input mesh to xatlas.
    xatlas::MeshDecl decl;
    decl.vertexCount         = static_cast<uint32_t>(mesh.positions.size());
    decl.vertexPositionData  = mesh.positions.data();
    decl.vertexPositionStride = static_cast<uint32_t>(sizeof(glm::vec3));
    if (!mesh.normals.empty() && mesh.normals.size() == mesh.positions.size()) {
        decl.vertexNormalData   = mesh.normals.data();
        decl.vertexNormalStride = static_cast<uint32_t>(sizeof(glm::vec3));
    }
    if (!mesh.uvs.empty() && mesh.uvs.size() == mesh.positions.size()) {
        decl.vertexUvData       = mesh.uvs.data();
        decl.vertexUvStride     = static_cast<uint32_t>(sizeof(glm::vec2));
    }
    decl.indexCount  = static_cast<uint32_t>(mesh.indices.size());
    decl.indexData   = mesh.indices.data();
    decl.indexFormat = xatlas::IndexFormat::UInt32;

    xatlas::AddMeshError err = xatlas::AddMesh(atlas, decl, 1);
    if (err != xatlas::AddMeshError::Success) {
        res.error = std::string("xatlas::AddMesh failed: ")
                  + xatlas::StringForEnum(err);
        xatlas::Destroy(atlas);
        return res;
    }
    xatlas::AddMeshJoin(atlas);

    // ---- Configure chart + pack options from our wrapper struct.
    xatlas::ChartOptions co;
    if (opts.maxChartArea > 0.0f)      co.maxChartArea      = opts.maxChartArea;
    if (opts.maxBoundaryLength > 0.0f) co.maxBoundaryLength = opts.maxBoundaryLength;
    co.normalDeviationWeight = opts.normalDeviationW;
    co.roundnessWeight       = opts.roundnessW;
    co.straightnessWeight    = opts.straightnessW;
    co.normalSeamWeight      = opts.normalSeamW;
    co.textureSeamWeight     = opts.textureSeamW;

    xatlas::PackOptions po;
    po.maxChartSize    = static_cast<uint32_t>(opts.maxAtlasSize);
    po.padding         = opts.bilinearPadding ? 1 : 0;
    po.texelsPerUnit   = opts.texelsPerUnit;
    po.resolution      = static_cast<uint32_t>(opts.atlasResolution);
    po.bilinear        = opts.bilinearPadding;
    po.blockAlign      = opts.blockAlign;
    po.bruteForce      = opts.bruteForcePack;

    xatlas::Generate(atlas, co, po);

    if (atlas->meshCount == 0 || atlas->meshes == nullptr) {
        res.error = "xatlas produced no output mesh";
        xatlas::Destroy(atlas);
        return res;
    }

    // ---- Translate the output back into our MeshData-shaped buffers.
    // xatlas may have split vertices at chart seams. Each output vertex
    // has a `xref` index pointing back at the original vertex array, so
    // we copy position/normal/uv0 from there. The new uv1 comes from the
    // packed atlas coordinates (in pixels) divided by atlas size.
    const xatlas::Mesh& outMesh = atlas->meshes[0];
    res.positions.resize(outMesh.vertexCount);
    res.normals.resize(outMesh.vertexCount);
    res.uvs0.resize(outMesh.vertexCount);
    res.uvs1.resize(outMesh.vertexCount);
    res.indices.resize(outMesh.indexCount);

    const bool haveNormals = !mesh.normals.empty()
                            && mesh.normals.size() == mesh.positions.size();
    const bool haveUv0     = !mesh.uvs.empty()
                            && mesh.uvs.size() == mesh.positions.size();

    const float invW = atlas->width  > 0 ? 1.0f / static_cast<float>(atlas->width)  : 0.0f;
    const float invH = atlas->height > 0 ? 1.0f / static_cast<float>(atlas->height) : 0.0f;

    for (uint32_t i = 0; i < outMesh.vertexCount; ++i) {
        const xatlas::Vertex& v = outMesh.vertexArray[i];
        uint32_t orig = v.xref;
        if (orig >= mesh.positions.size()) orig = 0;
        res.positions[i] = mesh.positions[orig];
        res.normals  [i] = haveNormals ? mesh.normals[orig] : glm::vec3(0, 0, 1);
        res.uvs0     [i] = haveUv0     ? mesh.uvs    [orig] : glm::vec2(0.0f);
        res.uvs1     [i] = glm::vec2(v.uv[0] * invW, v.uv[1] * invH);
    }
    std::memcpy(res.indices.data(), outMesh.indexArray,
                 sizeof(uint32_t) * outMesh.indexCount);

    res.chartCount  = static_cast<int>(outMesh.chartCount);
    res.atlasWidth  = static_cast<int>(atlas->width);
    res.atlasHeight = static_cast<int>(atlas->height);
    res.ok          = true;

    std::printf("[UvAtlas] xatlas done: in=%u verts → out=%u verts (%+d), "
                 "%u indices, %u charts, atlas %dx%d\n",
                 static_cast<uint32_t>(mesh.positions.size()),
                 outMesh.vertexCount,
                 static_cast<int>(outMesh.vertexCount) - static_cast<int>(mesh.positions.size()),
                 outMesh.indexCount, outMesh.chartCount,
                 res.atlasWidth, res.atlasHeight);

    xatlas::Destroy(atlas);
    return res;
}

// ---- Cache ----------------------------------------------------------------

// FNV-1a 64-bit over arbitrary bytes. Good enough for cache invalidation;
// not for cryptographic use.
static uint64_t fnv1a64(const void* data, size_t bytes, uint64_t seed = 1469598103934665603ULL) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint64_t h = seed;
    for (size_t i = 0; i < bytes; ++i) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

uint64_t meshFingerprint(const MeshData& mesh) {
    uint64_t h = 1469598103934665603ULL;
    uint32_t vc = static_cast<uint32_t>(mesh.positions.size());
    uint32_t ic = static_cast<uint32_t>(mesh.indices.size());
    h = fnv1a64(&vc, sizeof(vc), h);
    h = fnv1a64(&ic, sizeof(ic), h);
    if (!mesh.positions.empty())
        h = fnv1a64(mesh.positions.data(), mesh.positions.size() * sizeof(glm::vec3), h);
    if (!mesh.indices.empty())
        h = fnv1a64(mesh.indices.data(), mesh.indices.size() * sizeof(uint32_t), h);
    return h;
}

// On-disk format:
//   magic              "SGUVA1\0\0"   (8 bytes)
//   fingerprint        uint64
//   vertexCount        uint32
//   indexCount         uint32
//   chartCount         uint32
//   atlasWidth/Height  uint32 ×2
//   positions          vec3 × vertexCount
//   normals            vec3 × vertexCount
//   uvs0               vec2 × vertexCount
//   uvs1               vec2 × vertexCount
//   indices            uint32 × indexCount

// Cache format v2:
//   magic        "SGUVA2\0\0"  (8 bytes)
//   fingerprint  uint64
//   vCount       uint32   — full-mesh vertex count, 0 in proxy mode
//   iCount       uint32   — full-mesh index count,  0 in proxy mode
//   uv1Count     uint32   — uvs1 buffer count; in proxy mode equals
//                            dense mesh vertex count, in legacy equals
//                            vCount. Stored explicitly so the proxy
//                            pipeline can save uvs1 without bundling
//                            positions / indices.
//   chartCount   uint32
//   atlasW       uint32
//   atlasH       uint32
//   if (vCount   > 0):   positions(vec3 × vCount),
//                        normals(vec3 × vCount),
//                        uvs0(vec2 × vCount)
//   if (uv1Count > 0):   uvs1(vec2 × uv1Count)
//   if (iCount   > 0):   indices(uint32 × iCount)
//
// v1 cache files are bytewise incompatible and ignored on load — they
// were broken anyway (didn't save uvs1 in proxy mode), so no data of
// value is being thrown away.
static constexpr char kCacheMagic[8] = { 'S','G','U','V','A','2','\0','\0' };

bool saveAtlasCache(const std::string& path,
                     uint64_t fingerprint,
                     const UvAtlasResult& res)
{
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;

    auto write = [&](const void* p, size_t n) {
        f.write(static_cast<const char*>(p), static_cast<std::streamsize>(n));
    };

    write(kCacheMagic, sizeof(kCacheMagic));
    write(&fingerprint, sizeof(fingerprint));
    uint32_t vc  = static_cast<uint32_t>(res.positions.size());
    uint32_t ic  = static_cast<uint32_t>(res.indices.size());
    uint32_t u1c = static_cast<uint32_t>(res.uvs1.size());
    uint32_t cc  = static_cast<uint32_t>(res.chartCount);
    uint32_t aw  = static_cast<uint32_t>(res.atlasWidth);
    uint32_t ah  = static_cast<uint32_t>(res.atlasHeight);
    write(&vc,  sizeof(vc));
    write(&ic,  sizeof(ic));
    write(&u1c, sizeof(u1c));
    write(&cc,  sizeof(cc));
    write(&aw,  sizeof(aw));
    write(&ah,  sizeof(ah));
    if (vc > 0) {
        write(res.positions.data(), vc * sizeof(glm::vec3));
        write(res.normals.data(),   vc * sizeof(glm::vec3));
        write(res.uvs0.data(),      vc * sizeof(glm::vec2));
    }
    if (u1c > 0) {
        write(res.uvs1.data(),      u1c * sizeof(glm::vec2));
    }
    if (ic > 0) {
        write(res.indices.data(),   ic * sizeof(uint32_t));
    }
    return f.good();
}

bool loadAtlasCache(const std::string& path,
                     uint64_t expectedFingerprint,
                     UvAtlasResult& out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;

    auto read = [&](void* p, size_t n) {
        f.read(static_cast<char*>(p), static_cast<std::streamsize>(n));
        return static_cast<size_t>(f.gcount()) == n;
    };

    char magic[8] = {0};
    if (!read(magic, sizeof(magic))) return false;
    if (std::memcmp(magic, kCacheMagic, sizeof(kCacheMagic)) != 0) {
        std::fprintf(stderr,
            "[UvAtlas] Cache %s magic mismatch (legacy v1 or unknown format) — "
            "ignored. Will regenerate.\n", path.c_str());
        return false;
    }
    uint64_t fp = 0;
    if (!read(&fp, sizeof(fp))) return false;
    if (fp != expectedFingerprint) {
        std::fprintf(stderr,
            "[UvAtlas] Cache %s fingerprint mismatch (mesh changed). "
            "Will regenerate.\n", path.c_str());
        return false;
    }
    uint32_t vc=0, ic=0, u1c=0, cc=0, aw=0, ah=0;
    if (!read(&vc,  sizeof(vc)))  return false;
    if (!read(&ic,  sizeof(ic)))  return false;
    if (!read(&u1c, sizeof(u1c))) return false;
    if (!read(&cc,  sizeof(cc)))  return false;
    if (!read(&aw,  sizeof(aw)))  return false;
    if (!read(&ah,  sizeof(ah)))  return false;

    out.positions.resize(vc);
    out.normals.resize(vc);
    out.uvs0.resize(vc);
    out.uvs1.resize(u1c);
    out.indices.resize(ic);

    if (vc > 0) {
        if (!read(out.positions.data(), vc * sizeof(glm::vec3))) return false;
        if (!read(out.normals.data(),   vc * sizeof(glm::vec3))) return false;
        if (!read(out.uvs0.data(),      vc * sizeof(glm::vec2))) return false;
    }
    if (u1c > 0) {
        if (!read(out.uvs1.data(),      u1c * sizeof(glm::vec2))) return false;
    }
    if (ic > 0) {
        if (!read(out.indices.data(),   ic * sizeof(uint32_t))) return false;
    }
    out.chartCount  = static_cast<int>(cc);
    out.atlasWidth  = static_cast<int>(aw);
    out.atlasHeight = static_cast<int>(ah);
    out.ok          = true;
    return true;
}

} // namespace spacegen
