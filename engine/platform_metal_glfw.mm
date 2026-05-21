// GLFW <-> Cocoa <-> Metal bridge.
// This is the only .mm file in the project. It exists to:
//   1. Get the NSWindow underlying the GLFWwindow handle.
//   2. Set the NSWindow's content view to layer-backed.
//   3. Install a CAMetalLayer as that layer.
// Once attached, the rest of the engine uses Metal-cpp without touching Cocoa.

#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>

#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include "platform_metal_glfw.h"

namespace spacegen::platform {

double attachMetalLayerToWindow(GLFWwindow* window, CA::MetalLayer* layer) {
    NSWindow* nsWin = glfwGetCocoaWindow(window);
    NSView*   view  = [nsWin contentView];

    view.wantsLayer = YES;
    // Metal-cpp's CA::MetalLayer* is bit-compatible with CAMetalLayer*
    // (it is the same Objective-C object, accessed via the C++ wrapper).
    // The __bridge cast tells ARC we're not transferring ownership.
    view.layer = (__bridge CAMetalLayer*)layer;

    return [nsWin backingScaleFactor];
}

} // namespace spacegen::platform
