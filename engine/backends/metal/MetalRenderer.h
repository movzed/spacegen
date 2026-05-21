#pragma once

#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>

#include <glm/glm.hpp>
#include <glm/mat4x4.hpp>

#include <vector>

namespace spacegen {

struct Scene;
struct MeshData;

// M2-B: minimal Metal backend.
// Builds a flat-color pipeline, uploads scene meshes to GPU buffers, and
// renders them through the scene's camera matrices into a swapchain-provided
// color attachment with a managed depth attachment.
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
        MTL::Buffer* vertexBuffer = nullptr;  // float3 positions
        MTL::Buffer* indexBuffer  = nullptr;  // uint32 indices
        uint32_t     indexCount   = 0;
        glm::mat4    transform    = glm::mat4(1.0f);
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
    glm::mat4                  projection_   = glm::mat4(1.0f);
    glm::mat4                  view_         = glm::mat4(1.0f);
};

} // namespace spacegen
