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

    // Phase 1: begin ImGui frame, build dockspace if first run, allocate
    // (or reuse) the offscreen composition texture at the SCENE's authored
    // output resolution, and return it for the caller to render into.
    //
    // The texture's size is FIXED to the scene's camera resolution, NOT to
    // the central dock node's size — this preserves the camera's aspect
    // ratio regardless of how the operator resizes the panels. The
    // Composition window then fits-inside-with-letterboxing when it
    // displays the texture.
    CompositionTarget beginFrame(int sceneOutputW,
                                  int sceneOutputH,
                                  const FrameStats& stats);

    // Phase 2: draw panels and the composition Image, render ImGui draw data
    // into `swapchainTarget` on `cb`. Caller is responsible for present +
    // commit. Reads back the actual central node size and caches it so the
    // next beginFrame can size the offscreen texture exactly.
    // Scene is non-const because the Layer Rack panel mutates scene.bus
    // (add / remove layers via the "+ Beam" button etc.).
    void endFrame(MTL::CommandBuffer* cb,
                  MTL::Texture* swapchainTarget,
                  Scene& scene,
                  MetalRenderer& renderer);

    // Load the operator-chosen startup-default preset (presets/default.json
    // next to scene.json) into scene.bus, REPLACING the current layer stack.
    // No-op if no default preset exists. Idempotent: only acts the first time
    // it succeeds. Safe to call once after the bus is set up.
    //
    // The Workstation also calls this automatically on its first endFrame, so
    // main.cpp does NOT need to call it — but a one-liner after bus setup
    // (`workstation.loadDefaultPresetIfAny(scene);`) is supported if the
    // manager prefers the default applied before the first rendered frame.
    void loadDefaultPresetIfAny(Scene& scene);

private:
    void ensureOffscreen(int widthPx, int heightPx);
    void releaseOffscreen();

    MTL::Device*     device_           = nullptr; // non-owning
    MTL::PixelFormat offscreenFormat_  = MTL::PixelFormatBGRA8Unorm;
    MTL::Texture*    offscreen_        = nullptr;
    int              offscreenW_       = 0;
    int              offscreenH_       = 0;

    bool             layoutBuilt_      = false;
    FrameStats       currentStats_;

    // Panel visibility toggles.
    bool             showStats_        = true;
    bool             showLayers_       = true;
    bool             showInspector_    = true;
    bool             showModulators_   = true;
    bool             showUvAnalysis_   = true;
    bool             showPostFx_       = true;
    bool             showDemo_         = false;
    bool             panelStateLoaded_ = false;
    bool             defaultPresetLoaded_ = false;

    // Currently selected layer (by id) — drives the Inspector contents.
    uint32_t         selectedLayerId_  = 0;
};

} // namespace spacegen::gui
