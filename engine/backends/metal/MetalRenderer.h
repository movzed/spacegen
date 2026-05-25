#pragma once

#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>

#include <glm/glm.hpp>
#include <glm/mat4x4.hpp>

#include <vector>

namespace spacegen {

struct Scene;
struct MeshData;
struct Light;
struct RenderContext;
class  Bus;
class  StructureLayer;
class  BeamLayer;
class  DirectionalLightLayer;

// M3-B: layer-driven Metal backend.
// Owns the pipelines + offscreen depth + per-mesh GPU buffers. Exposes
// granular methods that each Layer calls to do its work. Walks a Bus of
// layers per frame.
class MetalRenderer {
public:
    MetalRenderer(MTL::Device* device, MTL::PixelFormat colorFormat);
    ~MetalRenderer();

    // Non-copyable, non-movable (Metal-cpp pointers are bare).
    MetalRenderer(const MetalRenderer&)            = delete;
    MetalRenderer& operator=(const MetalRenderer&) = delete;

    // Uploads all meshes from `scene` into GPU buffers. Stores camera
    // matrices for use during render. Idempotent.
    void loadScene(const Scene& scene);

    // Allocates / reallocates the depth texture if size changed.
    void onResize(int widthPixels, int heightPixels);

    // Walks `bus` and renders all enabled layers into ctx.colorTarget.
    // Each layer's render() calls back into this class via the granular
    // methods below.
    void renderFrame(RenderContext& ctx, Bus& bus);

    // ---- Per-layer render helpers (called from Layer::render) ----
    // Structure pass: PBR forward over all loaded meshes with the layer's
    // material + up to 16 spot lights + up to 4 directional lights + a
    // summed ambient color (from all enabled AmbientLightLayers in the bus).
    // LoadAction=Clear.
    void renderStructureMeshes(
        RenderContext& ctx,
        const StructureLayer& layer,
        const std::vector<const BeamLayer*>& spots,
        const std::vector<const DirectionalLightLayer*>& dirs,
        const glm::vec3& ambientColor,
        MTL::Texture* syphonTex    = nullptr,   // optional live video overlay
        float          syphonMix   = 0.0f,
        const glm::vec3& syphonTint = glm::vec3(1.0f),
        bool           syphonFlipY = false);

    // Hot-swap the GPU buffers for one mesh in-place. Used by the UV
    // Analysis panel to apply a freshly generated atlas without restart.
    // The new MeshData replaces positions/normals/uvs/uvs1/indices for
    // mesh `meshIdx`. Returns true on success.
    bool reloadMesh(size_t meshIdx, const MeshData& newMesh);

    // Public scene matrices (used by Layers when building their uniforms).
    const glm::mat4& projection()      const { return projection_; }
    const glm::mat4& view()            const { return view_; }
    const glm::vec3& cameraWorldPos()  const { return cameraWorldPos_; }

    // For introspection / logging
    size_t meshCount() const { return gpuMeshes_.size(); }
    size_t totalTriangles() const;

private:
    void buildPipeline();
    void releaseDepthTexture();
    void buildDefaultTexturesAndSampler();
    void releaseTexturePool();
    MTL::Texture* createTextureFromRgba(const uint8_t* rgba,
                                          int width, int height,
                                          bool isSRGB,
                                          const char* debugName);

    struct GpuMesh {
        MTL::Buffer* positionBuffer = nullptr;   // float3 positions
        MTL::Buffer* normalBuffer   = nullptr;   // float3 normals (may be null)
        MTL::Buffer* uvBuffer       = nullptr;   // float2 UV0 — materials
        MTL::Buffer* uv1Buffer      = nullptr;   // float2 UV1 — Syphon overlay
        MTL::Buffer* indexBuffer    = nullptr;   // uint32 indices
        uint32_t     indexCount     = 0;
        glm::mat4    transform      = glm::mat4(1.0f);
        int          materialIdx    = -1;        // -1 = default material
        std::string  name;
    };

    // GPU material: pointers to textures (owned by texturePool_) + factors.
    struct GpuMaterial {
        MTL::Texture* baseColorTex = nullptr;
        MTL::Texture* mrTex        = nullptr;    // metallicRoughness
        MTL::Texture* emissiveTex  = nullptr;
        glm::vec4 baseColorFactor  = glm::vec4(1.0f);
        glm::vec3 emissiveFactor   = glm::vec3(0.0f);
        float     metallicFactor   = 1.0f;
        float     roughnessFactor  = 1.0f;
    };

    MTL::Device*               device_       = nullptr; // non-owning
    MTL::PixelFormat           colorFormat_  = MTL::PixelFormatBGRA8Unorm;

    MTL::RenderPipelineState*  pipeline_     = nullptr;
    MTL::DepthStencilState*    depthState_   = nullptr;
    MTL::Texture*              depthTex_     = nullptr;
    int                        depthW_       = 0;
    int                        depthH_       = 0;

    MTL::SamplerState*         linearSampler_  = nullptr;
    MTL::Texture*              defaultWhite_   = nullptr;   // 1x1 sRGB white
    MTL::Texture*              defaultLinear_  = nullptr;   // 1x1 linear white
    MTL::Texture*              defaultBlack_   = nullptr;   // 1x1 linear black

    std::vector<MTL::Texture*> texturePool_;   // uploaded from Scene.textures
    std::vector<GpuMaterial>   gpuMaterials_;  // mirror of Scene.materials
    GpuMaterial                defaultMaterial_;

    std::vector<GpuMesh>       gpuMeshes_;
    glm::mat4                  projection_      = glm::mat4(1.0f);
    glm::mat4                  view_            = glm::mat4(1.0f);
    glm::vec3                  cameraWorldPos_  = glm::vec3(0.0f);

};

} // namespace spacegen
