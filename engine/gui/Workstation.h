#pragma once
// SpaceGen Workstation — owns the ImGui lifecycle and the layout.
//
// Layout model (Resolume/VDMX-class):
//   - A persistent dockspace covers the whole window.
//   - "Stats" docks LEFT, "Inspector" docks RIGHT.
//   - "Composition" is the CENTRAL dock node and shows the rendered scene
//     as an ImGui::Image. The scene is rendered to an offscreen Metal
//     texture that the Workstation owns and resizes to match the central
//     node's content region.
//   - User can re-arrange any panel; the central composition image follows
//     the new central node size next frame.
//
// Two-phase usage per frame (driven by main.cpp):
//   1. workstation.beginFrame(...) — begins the ImGui frame, builds the
//      dockspace layout on first call, and returns an offscreen texture
//      sized to the central node. The caller (renderer) draws the scene
//      into that texture.
//   2. workstation.endFrame(cb, swapchainTexture, scene, renderer) —
//      finishes the ImGui frame (panels + composition image overlaid on
//      top of the central node) and submits draw data into the swapchain
//      texture.

#include <Metal/Metal.hpp>

struct GLFWwindow;

namespace spacegen {
struct Scene;
class  MetalRenderer;
} // namespace spacegen

namespace spacegen::gui {

struct FrameStats {
    double fps         = 0.0;
    double frameTimeMs = 0.0;
    int    drawableW   = 0;
    int    drawableH   = 0;
};

struct CompositionTarget {
    MTL::Texture* texture = nullptr;  // valid until next beginFrame
    int           widthPx  = 0;        // in pixels (already × backing scale)
    int           heightPx = 0;
};

class Workstation {
public:
    Workstation(GLFWwindow* window,
                MTL::Device* device,
                MTL::PixelFormat colorFormat);
    ~Workstation();

    Workstation(const Workstation&)            = delete;
    Workstation& operator=(const Workstation&) = delete;

    // Phase 1: begin ImGui frame, build dockspace if first run, allocate /
    // resize the offscreen composition texture, and return it for the caller
    // to render into. The texture's size matches the current central dock
    // node's content region (in pixels, already accounting for backing scale).
    // On the very first frame an estimate is used; subsequent frames are
    // exact because the previous frame's composition size is cached.
    CompositionTarget beginFrame(int swapchainW,
                                  int swapchainH,
                                  const FrameStats& stats);

    // Phase 2: draw panels and the composition Image, render ImGui draw data
    // into `swapchainTarget` on `cb`. Caller is responsible for present +
    // commit. Reads back the actual central node size and caches it so the
    // next beginFrame can size the offscreen texture exactly.
    void endFrame(MTL::CommandBuffer* cb,
                  MTL::Texture* swapchainTarget,
                  const Scene& scene,
                  MetalRenderer& renderer);

private:
    void ensureOffscreen(int widthPx, int heightPx);
    void releaseOffscreen();

    MTL::Device*     device_           = nullptr; // non-owning
    MTL::PixelFormat offscreenFormat_  = MTL::PixelFormatBGRA8Unorm;
    MTL::Texture*    offscreen_        = nullptr;
    int              offscreenW_       = 0;
    int              offscreenH_       = 0;

    // Cached central-node size from the previous endFrame() — used to size
    // the offscreen at next beginFrame() so it always matches.
    int              cachedCompW_      = 0;
    int              cachedCompH_      = 0;

    bool             layoutBuilt_      = false;
    FrameStats       currentStats_;

    // Panel visibility toggles.
    bool             showStats_        = true;
    bool             showInspector_    = true;
    bool             showDemo_         = false;
};

} // namespace spacegen::gui
