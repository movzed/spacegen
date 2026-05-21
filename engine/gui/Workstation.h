#pragma once
// SpaceGen Workstation — owns the ImGui lifecycle and draws all panels.
//
// Public interface is pure C++. The implementation lives in Workstation.mm
// because ImGui's Metal backend (imgui_impl_metal.h) requires Objective-C++.

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

class Workstation {
public:
    // Initializes ImGui context, GLFW input backend, and Metal renderer
    // backend. The color format must match the swapchain pixel format used
    // when calling renderUI().
    Workstation(GLFWwindow* window,
                MTL::Device* device,
                MTL::PixelFormat colorFormat);
    ~Workstation();

    Workstation(const Workstation&)            = delete;
    Workstation& operator=(const Workstation&) = delete;

    // Builds one ImGui frame: NewFrame, drawPanels, Render. Call exactly once
    // per frame, after the main structure pass has rendered (so the panels
    // overlay onto the structure).
    //
    // Then call submit() with the same colorTarget to encode ImGui's draw
    // data into a separate render pass on `cb`.
    void buildAndSubmit(MTL::CommandBuffer* cb,
                        MTL::Texture* colorTarget,
                        const Scene& scene,
                        MetalRenderer& renderer,
                        const FrameStats& stats);
};

} // namespace spacegen::gui
