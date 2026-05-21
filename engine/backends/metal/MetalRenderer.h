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

// M2-C1: PBR forward rendering with realtime lights.
// Builds a PBR pipeline (GGX + Lambert + Schlick), uploads scene meshes
// (positions + normals + indices) and renders them through the scene's
// camera with up to one Directional light (multi-light is M2-C2).
class MetalRenderer {
public:
    MetalRenderer(MTL::Device* device, MTL::PixelFormat colorFormat);
    ~MetalRenderer();

    // Non-copyable, non-movable (Metal-cpp pointers are bare).
    MetalRenderer(const MetalRenderer&)            = delete;
    MetalRenderer& operator=(const MetalRenderer&) = delete;

    // Uploads all meshes from `scene` into GPU buffers. Stores camera matrices
    // for use during renderFrame(). Idempotent (re-loading replaces previous
    // GPU state).
    void loadScene(const Scene& scene);

    // Allocates / reallocates the depth texture if size changed.
    void onResize(int widthPixels, int heightPixels);

    // Encodes one frame onto `cmdBuf`. Reads from `colorTarget` (a drawable
    // texture obtained via CAMetalLayer::nextDrawable()) and writes color +
    // managed depth. Caller owns commit/present.
    void renderFrame(MTL::CommandBuffer* cmdBuf,
                     MTL::Texture* colorTarget,
                     double elapsedSeconds);

    // For introspection / logging
    size_t meshCount() const { return gpuMeshes_.size(); }
    size_t totalTriangles() const;

private:
    void buildPipeline();
    void releaseDepthTexture();

    struct GpuMesh {
        MTL::Buffer* positionBuffer = nullptr;   // float3 positions
        MTL::Buffer* normalBuffer   = nullptr;   // float3 normals (may be null)
        MTL::Buffer* indexBuffer    = nullptr;   // uint32 indices
        uint32_t     indexCount     = 0;
        glm::mat4    transform      = glm::mat4(1.0f);
        std::string  name;
    };

    MTL::Device*               device_       = nullptr; // non-owning
    MTL::PixelFormat           colorFormat_  = MTL::PixelFormatBGRA8Unorm;

    MTL::RenderPipelineState*  pipeline_     = nullptr;
    MTL::DepthStencilState*    depthState_   = nullptr;
    MTL::Texture*              depthTex_     = nullptr;
    int                        depthW_       = 0;
    int                        depthH_       = 0;

    std::vector<GpuMesh>       gpuMeshes_;
    glm::mat4                  projection_      = glm::mat4(1.0f);
    glm::mat4                  view_            = glm::mat4(1.0f);
    glm::vec3                  cameraWorldPos_  = glm::vec3(0.0f);

    // M2-C1: single hardcoded light, exposed publicly so main.cpp can poke
    // direction/color/intensity per frame. M2-C2 will move this through the
    // parameter graph + ImGui binding.
public:
    glm::vec3                  lightDirection   = glm::vec3(0.4f, -0.6f, -0.6f);
    glm::vec3                  lightColor       = glm::vec3(1.0f, 0.96f, 0.92f);
    float                      lightIntensity   = 1.6f;
    glm::vec3                  baseColor        = glm::vec3(0.72f);
    float                      roughness        = 0.55f;
    float                      metallic         = 0.0f;
    float                      ambient          = 0.04f;
};

} // namespace spacegen
