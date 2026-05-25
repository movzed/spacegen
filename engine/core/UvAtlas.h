#pragma once
// UvAtlas — wrapper around the xatlas library that generates a high-quality
// SyphonUV layer from a mesh's positions + indices.
//
// xatlas detects "charts" (clusters of faces with similar normals / curvature),
// parameterizes each chart with LSCM (Least Squares Conformal Maps, which
// preserves angles), and packs the resulting islands into [0, 1] with uniform
// texel density. This is the standard solution used by Unity, Unreal Engine,
// Godot, Bevy, etc. for lightmap / texture-atlas UV generation.
//
// The output mesh may have MORE vertices than the input — xatlas splits
// vertices at chart seams. All per-vertex attributes (positions, normals,
// existing UV0) are duplicated through the xref table to keep the mesh
// renderable as a single indexed primitive.

#include "Scene.h"

#include <string>

namespace spacegen {

// Result of generating an atlas for a mesh. The output is a full
// replacement: the engine should swap the mesh's positions/normals/indices
// for the regenerated ones, and assign uvs1 from `uvs1`.
struct UvAtlasResult {
    bool                   ok = false;
    std::string            error;          // human-readable failure reason
    int                    chartCount = 0; // number of UV islands generated
    int                    atlasWidth = 0; // packed atlas width in pixels
    int                    atlasHeight = 0;

    // Regenerated mesh attributes. Sizes may differ from the input because
    // xatlas duplicates vertices at chart seams.
    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> normals;
    std::vector<glm::vec2> uvs0;     // original UV0, remapped (or empty)
    std::vector<glm::vec2> uvs1;     // new SyphonUV in [0, 1]
    std::vector<uint32_t>  indices;
};

// Options for the chart detection / packing stages. Defaults are tuned
// for projection-mapping live video on architectural meshes — large charts,
// good packing density, minimal stretch.
struct UvAtlasOptions {
    // Chart options
    float maxChartArea        = 0.0f;   // 0 = unbounded; let xatlas auto-pick
    float maxBoundaryLength   = 0.0f;   // 0 = unbounded
    float normalDeviationW    = 2.0f;   // weight on normal deviation cost
    float roundnessW          = 0.01f;  // weight on chart roundness
    float straightnessW       = 6.0f;   // weight on boundary straightness
    float normalSeamW         = 4.0f;   // weight on normal seam preservation
    float textureSeamW        = 0.5f;   // weight on existing UV seam preserv.

    // Pack options
    int   atlasResolution     = 0;      // 0 = auto from texelsPerUnit
    int   maxAtlasSize        = 4096;
    float texelsPerUnit       = 0.0f;   // 0 = auto-fit packing into one atlas
    bool  bilinearPadding     = true;   // 1-pixel padding so bilinear sampling
                                         // doesn't bleed from neighbor charts
    bool  blockAlign          = false;
    bool  bruteForcePack      = false;  // slower, better packing density
};

// Run xatlas on a mesh. Single-mesh: SpaceGen scenes have one structure mesh
// in the v1 schema. Returns a fully rebuilt mesh in the result.
//
// `progressCb` is optional — called with (stagePct, stageName) during run.
// Use it to drive an ImGui progress bar. May be empty.
using ProgressFn = void(*)(int progressPct, const char* stageName, void* user);
UvAtlasResult generateAtlas(const MeshData& mesh,
                             const UvAtlasOptions& opts = {},
                             ProgressFn progressCb = nullptr,
                             void* progressUser = nullptr);

// ---- Disk cache for the atlas result. Stored next to scene.json as a
// binary blob so we can skip the multi-second/minute xatlas run on most
// scene loads. The cache key is the mesh's vertex/triangle count + a
// hash of the position bytes; if those change, the cache is invalidated.

// Compute a deterministic 64-bit fingerprint of the mesh inputs (positions,
// normals, original UV0, indices). The fingerprint changes if anything that
// would affect the atlas changes.
uint64_t meshFingerprint(const MeshData& mesh);

bool saveAtlasCache(const std::string& path,
                     uint64_t inputFingerprint,
                     const UvAtlasResult& res);

bool loadAtlasCache(const std::string& path,
                     uint64_t expectedFingerprint,
                     UvAtlasResult& out);

} // namespace spacegen
