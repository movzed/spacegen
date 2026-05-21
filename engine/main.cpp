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
#include "core/Layer.h"
#include "core/StructureLayer.h"
#include "core/BeamLayer.h"
#include "core/DirectionalLightLayer.h"
#include "core/AmbientLightLayer.h"
#include "backends/metal/MetalRenderer.h"
#include "gui/Workstation.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string>

namespace {

constexpr char kWindowTitle[] = "SpaceGen — M3-B.5 (multi-fixture rigs + per-fixture color + chase)";

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

    // Populate the bus with the default starter layers. Everything except
    // Structure starts disabled so the operator sees a dark stage and adds
    // light explicitly — no implicit gray fill on the relief.
    scene.bus.add<spacegen::StructureLayer>();
    {
        auto* a = scene.bus.add<spacegen::AmbientLightLayer>();
        a->name      = "Ambient";
        a->intensity = 0.02f;
        a->state     = spacegen::LayerState::Disabled;
    }
    {
        auto* d = scene.bus.add<spacegen::DirectionalLightLayer>();
        d->name      = "Key fill";
        d->panDeg    = 35.0f;
        d->tiltDeg   = -55.0f;
        d->intensity = 1.0f;
        d->state     = spacegen::LayerState::Disabled;
    }
    {
        auto* b = scene.bus.add<spacegen::BeamLayer>();
        b->name         = "Spot 1";
        b->followCamera = true;
        b->origin       = glm::vec3(scene.camera.world[3]);
        b->panDeg       = 0.0f;
        b->tiltDeg      = 0.0f;
        b->intensity    = 4.0f;
        b->innerDeg     = 12.0f;
        b->outerDeg     = 22.0f;
    }

    // ImGui Workstation (overlays panels on top of the rendered scene).
    spacegen::gui::Workstation workstation(window, device, kColorFmt);

    // ============================================================
    // Render loop
    // ============================================================
    auto t0     = std::chrono::steady_clock::now();
    auto tFrame = t0;
    int  frameCounter = 0;
    double smoothedFps     = 60.0;
    double smoothedFrameMs = 16.67;

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

            // Phase 1: workstation builds ImGui frame, returns an offscreen
            // texture sized to the central dock node. Bus renders there.
            spacegen::gui::FrameStats stats;
            stats.fps         = smoothedFps;
            stats.frameTimeMs = smoothedFrameMs;
            stats.drawableW   = fbW;
            stats.drawableH   = fbH;
            auto target = workstation.beginFrame(
                scene.outputWidth, scene.outputHeight, stats);

            spacegen::RenderContext ctx;
            ctx.renderer       = renderer;
            ctx.scene          = &scene;
            ctx.cmdBuf         = cb;
            ctx.colorTarget    = target.texture;
            ctx.width          = target.widthPx;
            ctx.height         = target.heightPx;
            ctx.projection     = renderer->projection();
            ctx.view           = renderer->view();
            ctx.cameraWorldPos = renderer->cameraWorldPos();
            // Camera forward in world space = -Z of camera's local frame.
            ctx.cameraForward  = -glm::vec3(scene.camera.world[2]);
            ctx.elapsedSeconds = elapsed;
            ctx.frameIndex     = frameCounter;

            renderer->renderFrame(ctx, scene.bus);

            // Phase 2: workstation finishes the ImGui frame (panels +
            // composition Image) and submits draw data to the swapchain.
            workstation.endFrame(cb, drawable->texture(),
                                  scene, *renderer);

            cb->presentDrawable(drawable);
            cb->commit();
        }
        framePool->release();
        frameCounter++;

        const auto now = std::chrono::steady_clock::now();
        double sinceLast = std::chrono::duration<double>(now - tFrame).count();
        if (sinceLast >= 0.5) {
            smoothedFps    = frameCounter / sinceLast;
            smoothedFrameMs = sinceLast * 1000.0 / frameCounter;
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
