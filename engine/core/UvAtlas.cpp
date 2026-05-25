#include "UvAtlas.h"

#include <xatlas.h>

#include <cstdio>
#include <cstring>
#include <fstream>

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
    if (b && b->fn) {
        const char* name = "?";
        switch (category) {
            case xatlas::ProgressCategory::AddMesh:        name = "Validating geometry"; break;
            case xatlas::ProgressCategory::ComputeCharts:  name = "Detecting charts"; break;
            case xatlas::ProgressCategory::PackCharts:     name = "Packing atlas"; break;
            case xatlas::ProgressCategory::BuildOutputMeshes: name = "Building output"; break;
        }
        b->fn(progress, name, b->user);
    }
    // Return true to continue, false to cancel. We never cancel from here.
    return true;
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

    xatlas::SetPrint(xatlasPrintForward, /*verbose=*/false);
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

static constexpr char kCacheMagic[8] = { 'S','G','U','V','A','1','\0','\0' };

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
    uint32_t vc = static_cast<uint32_t>(res.positions.size());
    uint32_t ic = static_cast<uint32_t>(res.indices.size());
    uint32_t cc = static_cast<uint32_t>(res.chartCount);
    uint32_t aw = static_cast<uint32_t>(res.atlasWidth);
    uint32_t ah = static_cast<uint32_t>(res.atlasHeight);
    write(&vc, sizeof(vc));
    write(&ic, sizeof(ic));
    write(&cc, sizeof(cc));
    write(&aw, sizeof(aw));
    write(&ah, sizeof(ah));
    if (vc > 0) {
        write(res.positions.data(), vc * sizeof(glm::vec3));
        write(res.normals.data(),   vc * sizeof(glm::vec3));
        write(res.uvs0.data(),      vc * sizeof(glm::vec2));
        write(res.uvs1.data(),      vc * sizeof(glm::vec2));
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
    if (std::memcmp(magic, kCacheMagic, sizeof(kCacheMagic)) != 0) return false;
    uint64_t fp = 0;
    if (!read(&fp, sizeof(fp))) return false;
    if (fp != expectedFingerprint) {
        std::fprintf(stderr,
            "[UvAtlas] Cache %s exists but fingerprint mismatch (mesh changed). "
            "Will regenerate.\n", path.c_str());
        return false;
    }
    uint32_t vc=0, ic=0, cc=0, aw=0, ah=0;
    if (!read(&vc, sizeof(vc))) return false;
    if (!read(&ic, sizeof(ic))) return false;
    if (!read(&cc, sizeof(cc))) return false;
    if (!read(&aw, sizeof(aw))) return false;
    if (!read(&ah, sizeof(ah))) return false;

    out.positions.resize(vc);
    out.normals.resize(vc);
    out.uvs0.resize(vc);
    out.uvs1.resize(vc);
    out.indices.resize(ic);

    if (vc > 0) {
        if (!read(out.positions.data(), vc * sizeof(glm::vec3))) return false;
        if (!read(out.normals.data(),   vc * sizeof(glm::vec3))) return false;
        if (!read(out.uvs0.data(),      vc * sizeof(glm::vec2))) return false;
        if (!read(out.uvs1.data(),      vc * sizeof(glm::vec2))) return false;
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
