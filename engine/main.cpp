// SpaceGen — engine entry point.
//
// Stage M2-A milestone:
//   - GLFW window opens at 1280x720 (resizable, Retina-aware).
//   - Metal device acquired, CAMetalLayer attached to the window.
//   - Render loop clears each frame to a slowly-animated color so we can
//     visually confirm the loop is running.
//   - Exits cleanly on window close.
//
// No scene loading, no PBR shader, no structure mesh yet — those land in
// the next M2 stages. This file proves the toolchain end-to-end.

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "platform_metal_glfw.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {

constexpr int  kInitialWidth  = 1280;
constexpr int  kInitialHeight = 720;
constexpr char kWindowTitle[] = "SpaceGen — M2-A";

void glfwErrorCallback(int code, const char* desc) {
    std::fprintf(stderr, "[GLFW error %d] %s\n", code, desc);
}

} // namespace

int main(int /*argc*/, char** /*argv*/) {
    // ============================================================
    // GLFW init
    // ============================================================
    glfwSetErrorCallback(glfwErrorCallback);

    if (!glfwInit()) {
        std::fprintf(stderr, "[SpaceGen] Failed to initialize GLFW\n");
        return EXIT_FAILURE;
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);   // we own the graphics context
    glfwWindowHint(GLFW_RESIZABLE,  GLFW_TRUE);
    glfwWindowHint(GLFW_COCOA_RETINA_FRAMEBUFFER, GLFW_TRUE);

    GLFWwindow* window = glfwCreateWindow(
        kInitialWidth, kInitialHeight, kWindowTitle, nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "[SpaceGen] Failed to create GLFW window\n");
        glfwTerminate();
        return EXIT_FAILURE;
    }

    // ============================================================
    // Metal init
    // ============================================================
    NS::AutoreleasePool* sessionPool = NS::AutoreleasePool::alloc()->init();

    MTL::Device* device = MTL::CreateSystemDefaultDevice();
    if (!device) {
        std::fprintf(stderr, "[SpaceGen] No Metal device available\n");
        glfwDestroyWindow(window);
        glfwTerminate();
        return EXIT_FAILURE;
    }
    std::printf("[SpaceGen] Metal device: %s\n",
                device->name()->utf8String());

    MTL::CommandQueue* commandQueue = device->newCommandQueue();

    CA::MetalLayer* metalLayer = CA::MetalLayer::layer()->retain();
    metalLayer->setDevice(device);
    metalLayer->setPixelFormat(MTL::PixelFormatBGRA8Unorm);
    metalLayer->setFramebufferOnly(true);

    double scale = spacegen::platform::attachMetalLayerToWindow(window, metalLayer);
    std::printf("[SpaceGen] Backing scale factor: %.2fx\n", scale);

    int fbW = 0, fbH = 0;
    glfwGetFramebufferSize(window, &fbW, &fbH);
    metalLayer->setDrawableSize(CGSizeMake(fbW, fbH));
    std::printf("[SpaceGen] Initial drawable size: %dx%d (logical %dx%d)\n",
                fbW, fbH, kInitialWidth, kInitialHeight);

    // ============================================================
    // Render loop
    // ============================================================
    auto t0         = std::chrono::steady_clock::now();
    auto tFrame     = t0;
    int  frameCount = 0;

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        // ESC closes the window for convenience during dev.
        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        }

        // Handle resize.
        int newFbW = 0, newFbH = 0;
        glfwGetFramebufferSize(window, &newFbW, &newFbH);
        if (newFbW != fbW || newFbH != fbH) {
            fbW = newFbW;
            fbH = newFbH;
            metalLayer->setDrawableSize(CGSizeMake(fbW, fbH));
        }

        NS::AutoreleasePool* framePool = NS::AutoreleasePool::alloc()->init();

        CA::MetalDrawable* drawable = metalLayer->nextDrawable();
        if (drawable) {
            // Slow animated clear color — proof of life that the loop runs.
            const auto now      = std::chrono::steady_clock::now();
            const double elapsed = std::chrono::duration<double>(now - t0).count();
            const double r = 0.06 + 0.05 * std::sin(elapsed * 0.7);
            const double g = 0.07 + 0.05 * std::sin(elapsed * 0.9 + 1.0);
            const double b = 0.12 + 0.06 * std::sin(elapsed * 1.1 + 2.0);

            MTL::RenderPassDescriptor* rpd =
                MTL::RenderPassDescriptor::renderPassDescriptor();
            MTL::RenderPassColorAttachmentDescriptor* color =
                rpd->colorAttachments()->object(0);
            color->setTexture(drawable->texture());
            color->setLoadAction(MTL::LoadActionClear);
            color->setStoreAction(MTL::StoreActionStore);
            color->setClearColor(MTL::ClearColor(r, g, b, 1.0));

            MTL::CommandBuffer* cb = commandQueue->commandBuffer();
            MTL::RenderCommandEncoder* enc = cb->renderCommandEncoder(rpd);
            // No geometry yet — clear-only pass. Add draws in M2-B.
            enc->endEncoding();

            cb->presentDrawable(drawable);
            cb->commit();
        }

        framePool->release();
        frameCount++;

        // Print frame stats once per second so we can confirm framerate.
        const auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(now - tFrame).count() >= 1.0) {
            std::printf("[SpaceGen] %d fps  (drawable %dx%d)\n",
                        frameCount, fbW, fbH);
            frameCount = 0;
            tFrame = now;
        }
    }

    // ============================================================
    // Cleanup
    // ============================================================
    metalLayer->release();
    commandQueue->release();
    device->release();
    sessionPool->release();

    glfwDestroyWindow(window);
    glfwTerminate();

    std::printf("[SpaceGen] Bye.\n");
    return EXIT_SUCCESS;
}
