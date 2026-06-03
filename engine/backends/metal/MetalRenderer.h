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
struct VirtualSpot;
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
        bool           syphonFlipY = false,
        // Tier 2: stretch heatmap diagnostic
        bool           showHeatmap = false,
        int            heatmapMetric = 0,
        int            heatmapUV     = 1,
        float          projectorOnFlatMix       = 0.0f,
        float          projectorFlatnessThreshold = 0.05f,
        // LightClonerLayer expansion — virtual spots appended to the
        // existing spot array (kMaxSpots ceiling shared).
        const std::vector<VirtualSpot>& virtualSpots = {});

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

    // ---- Helpers for the effect layers (added 2026-05 alongside the
    //      11 agent-designed effects under engine/effects/). They expose
    //      the bits of MetalRenderer state that those effects need
    //      without making them friend-class everything. ----

    // Post-FX ping-pong: returns a partner texture matching
    // ctx.colorTarget (size + format) the effect can render into.
    // Lazily allocated, reused across frames. After the effect renders
    // into it, call swapPingPong(ctx) — ctx.colorTarget is reassigned
    // to the new texture, so the next layer down the bus sees the
    // post-FX result as its input.
    MTL::Texture*      acquirePingPongTarget(RenderContext& ctx);
    void               swapPingPong(RenderContext& ctx);
    MTL::SamplerState* postFxSampler();

    // Compute / particle effects need raw access to the underlying
    // Metal device and the depth texture used by the structure pass.
    // Returned as void* / MTL::Texture* so callers can include whatever
    // subset of Metal-cpp headers they want.
    void*              devicePublic()  const { return device_; }
    MTL::Texture*      depthTexturePublic() const { return depthTex_; }

    // ---- Volumetric beam render path (used by VolumetricBeamLayer).
    //      Layout matches what the agent-designed VolumetricBeamLayer.cpp
    //      packs at the call site (see engine/effects/volumetric_beams/).
    //      The implementation in MetalRenderer.cpp is a stub by default —
    //      the projection-mapping principle ("no light cones visible in
    //      air, only the projection on the stage") makes this effect
    //      DEFAULT-OFF in production. Enable + complete the raymarch in
    //      MetalRenderer::renderVolumetricBeams when intentionally going
    //      against the rule.
    struct VolumetricSpotPacked {
        glm::vec4 posIntensity;    // .xyz pos, .w intensity
        glm::vec4 dirRange;        // .xyz dir (FROM light), .w range
        glm::vec4 colorInner;      // .rgb color, .a innerCos
        glm::vec4 outerMul;        // .x outerCos
    };
    struct VolumetricUniforms {
        const VolumetricSpotPacked* spots          = nullptr;
        int                         spotCount      = 0;
        float                       density        = 0.0f;
        float                       anisotropy     = 0.0f;
        int                         sampleCount    = 32;
        float                       jitterStrength = 0.5f;
        glm::vec3                   tint           {1.0f};
        bool                        beerLambert    = false;
        float                       layerOpacity   = 1.0f;
    };
    void               renderVolumetricBeams(RenderContext& ctx,
                                              const VolumetricUniforms& u);

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
