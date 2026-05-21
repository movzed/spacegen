// SpaceGen — engine entry point.
//
// M2-B milestone:
//   - GLFW window + CAMetalLayer (carried over from M2-A).
//   - Loads a Blender export folder via Scene::loadFromFolder().
//   - MetalRenderer uploads each mesh, renders flat-color through the
//     scene's camera matrices with depth-test enabled.
//   - Visual success criterion: silhouette of the structure matches
//     `preview.png` in the same export folder.
//
// Usage:
//   ./spacegen [path/to/export_folder]
// If no path is given, falls back to ../examples/stage_ok_2 (relative to cwd
// when launched from engine/).

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "platform_metal_glfw.h"
#include "core/Scene.h"
#include "backends/metal/MetalRenderer.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string>

namespace {

constexpr char kWindowTitle[] = "SpaceGen — M2-B";

void glfwErrorCallback(int code, const char* desc) {
    std::fprintf(stderr, "[GLFW error %d] %s\n", code, desc);
}

std::string defaultScenePath() {
    // Try a few sensible locations relative to where the binary may be run.
    std::filesystem::path candidates[] = {
        "../examples/stage_ok_2",        // run from engine/build
        "examples/stage_ok_2",            // run from engine
        "../../examples/stage_ok_2",      // run from engine/build/foo
        "/Users/fpf/Desktop/spacegen/examples/stage_ok_2",
    };
    for (const auto& c : candidates) {
        if (std::filesystem::exists(c / "scene.json")) {
            return std::filesystem::absolute(c).string();
        }
    }
    return std::string{};
}

} // namespace

int main(int argc, char** argv) {
    // ============================================================
    // Resolve scene path
    // ============================================================
    std::string scenePath = (argc > 1) ? argv[1] : defaultScenePath();
    if (scenePath.empty()) {
        std::fprintf(stderr,
            "[SpaceGen] No scene path given and default not found.\n"
            "Usage: %s <path/to/export_folder>\n", argv[0]);
        return EXIT_FAILURE;
    }
    std::printf("[SpaceGen] Scene folder: %s\n", scenePath.c_str());

    // ============================================================
    // Load scene (before Metal so window can size to scene resolution)
    // ============================================================
    spacegen::Scene scene;
    try {
        scene = spacegen::Scene::loadFromFolder(scenePath);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[SpaceGen] Scene load failed: %s\n", e.what());
        return EXIT_FAILURE;
    }
    std::printf("[SpaceGen] Loaded scene: schema=%d, render=%dx%d, meshes=%zu, "
                 "source=%s\n",
                 scene.schemaVersion, scene.outputWidth, scene.outputHeight,
                 scene.meshes.size(), scene.sourceBlend.c_str());

    size_t totalV = 0, totalT = 0;
    for (const auto& m : scene.meshes) {
        totalV += m.positions.size();
        totalT += m.indices.size() / 3;
    }
    std::printf("[SpaceGen] Geometry: %zu verts, %zu tris across %zu meshes\n",
                 totalV, totalT, scene.meshes.size());
    std::printf("[SpaceGen] Camera: '%s'  FOV %.2fx%.2f deg  focal %.1f mm\n",
                 scene.camera.name.c_str(),
                 scene.camera.fovXRad * 180.0f / 3.14159265f,
                 scene.camera.fovYRad * 180.0f / 3.14159265f,
                 scene.camera.focalLengthMM);

    // ============================================================
    // GLFW init
    // ============================================================
    glfwSetErrorCallback(glfwErrorCallback);
    if (!glfwInit()) {
        std::fprintf(stderr, "[SpaceGen] glfwInit() failed\n");
        return EXIT_FAILURE;
    }

    // Open the window at half the export resolution so it fits on screen.
    // Drawable size still matches the framebuffer (Retina-aware) so the
    // projection aspect remains correct.
    int winW = scene.outputWidth  / 2;
    int winH = scene.outputHeight / 2;
    if (winW < 320) winW = 320;
    if (winH < 240) winH = 240;

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE,  GLFW_TRUE);
    glfwWindowHint(GLFW_COCOA_RETINA_FRAMEBUFFER, GLFW_TRUE);

    GLFWwindow* window = glfwCreateWindow(winW, winH, kWindowTitle,
                                           nullptr, nullptr);
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
        std::fprintf(stderr, "[SpaceGen] No Metal device\n");
        glfwDestroyWindow(window);
        glfwTerminate();
        return EXIT_FAILURE;
    }
    std::printf("[SpaceGen] Metal device: %s\n",
                 device->name()->utf8String());

    MTL::CommandQueue* commandQueue = device->newCommandQueue();

    constexpr MTL::PixelFormat kColorFmt = MTL::PixelFormatBGRA8Unorm;

    CA::MetalLayer* metalLayer = CA::MetalLayer::layer()->retain();
    metalLayer->setDevice(device);
    metalLayer->setPixelFormat(kColorFmt);
    metalLayer->setFramebufferOnly(true);

    double scale = spacegen::platform::attachMetalLayerToWindow(window,
                                                                  metalLayer);
    std::printf("[SpaceGen] Backing scale: %.2fx\n", scale);

    int fbW = 0, fbH = 0;
    glfwGetFramebufferSize(window, &fbW, &fbH);
    metalLayer->setDrawableSize(CGSizeMake(fbW, fbH));

    // ============================================================
    // Renderer
    // ============================================================
    spacegen::MetalRenderer* renderer = nullptr;
    try {
        renderer = new spacegen::MetalRenderer(device, kColorFmt);
        renderer->loadScene(scene);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[SpaceGen] Renderer init failed: %s\n", e.what());
        if (renderer) delete renderer;
        metalLayer->release();
        commandQueue->release();
        device->release();
        glfwDestroyWindow(window);
        glfwTerminate();
        return EXIT_FAILURE;
    }
    std::printf("[SpaceGen] Renderer: %zu meshes, %zu triangles total on GPU\n",
                 renderer->meshCount(), renderer->totalTriangles());

    // ============================================================
    // Render loop
    // ============================================================
    auto t0     = std::chrono::steady_clock::now();
    auto tFrame = t0;
    int  frameCounter = 0;

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        }

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
            const auto   now     = std::chrono::steady_clock::now();
            const double elapsed = std::chrono::duration<double>(now - t0).count();

            MTL::CommandBuffer* cb = commandQueue->commandBuffer();
            renderer->renderFrame(cb, drawable->texture(), elapsed);
            cb->presentDrawable(drawable);
            cb->commit();
        }
        framePool->release();
        frameCounter++;

        const auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(now - tFrame).count() >= 1.0) {
            std::printf("[SpaceGen] %d fps  (drawable %dx%d, %zu tris)\n",
                         frameCounter, fbW, fbH, renderer->totalTriangles());
            frameCounter = 0;
            tFrame = now;
        }
    }

    // ============================================================
    // Cleanup
    // ============================================================
    delete renderer;
    metalLayer->release();
    commandQueue->release();
    device->release();
    sessionPool->release();

    glfwDestroyWindow(window);
    glfwTerminate();

    std::printf("[SpaceGen] Bye.\n");
    return EXIT_SUCCESS;
}
