#pragma once
// Bridge between GLFW (Cocoa NSWindow) and Metal (CAMetalLayer).
// Only this header + its .mm file are platform-specific Cocoa code;
// everything else in the engine uses pure C++ via Metal-cpp.

struct GLFWwindow;
namespace CA { class MetalLayer; }

namespace spacegen::platform {

// Attaches a CAMetalLayer to the GLFW window's content view.
// After this call, the Metal layer renders into the window.
// Returns the window's backing scale factor (1.0 or 2.0 for Retina displays),
// which the caller should multiply the framebuffer size by when calling
// CAMetalLayer::setDrawableSize().
double attachMetalLayerToWindow(GLFWwindow* window, CA::MetalLayer* layer);

} // namespace spacegen::platform
