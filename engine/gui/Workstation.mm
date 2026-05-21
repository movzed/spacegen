// Workstation.mm — ObjC++ implementation of the ImGui workstation.
//
// This file is the bridge between Metal-cpp (our render layer) and the
// ImGui Metal backend (which uses ObjC types). The __bridge casts work
// because Metal-cpp pointers and Objective-C ids point at the same objects.

#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_metal.h"

#include "Workstation.h"
#include "../core/Scene.h"
#include "../backends/metal/MetalRenderer.h"

#include <cstdio>

namespace spacegen::gui {

namespace {

void styleSpaceGen() {
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding    = 4.0f;
    s.FrameRounding     = 3.0f;
    s.GrabRounding      = 3.0f;
    s.WindowPadding     = ImVec2(10.0f, 10.0f);
    s.FramePadding      = ImVec2(8.0f, 4.0f);
    s.ItemSpacing       = ImVec2(8.0f, 6.0f);
    s.WindowBorderSize  = 1.0f;
    s.FrameBorderSize   = 0.0f;
    s.TabRounding       = 3.0f;
}

// ---- panel helpers ----

void drawMainMenuBar(bool& showStats, bool& showInspector, bool& showDemo) {
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            ImGui::MenuItem("New scene...",   nullptr, false, false);
            ImGui::MenuItem("Open scene...",  nullptr, false, false);
            ImGui::Separator();
            ImGui::MenuItem("Save preset...", nullptr, false, false);
            ImGui::Separator();
            if (ImGui::MenuItem("Quit", "ESC")) {
                // Caller handles via the existing ESC binding; menu is hint.
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            ImGui::MenuItem("Stats",     nullptr, &showStats);
            ImGui::MenuItem("Inspector", nullptr, &showInspector);
            ImGui::Separator();
            ImGui::MenuItem("ImGui Demo", nullptr, &showDemo);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help")) {
            ImGui::MenuItem("SpaceGen M2-C2", nullptr, false, false);
            ImGui::MenuItem("github.com/movzed/spacegen", nullptr, false, false);
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }
}

void drawStatsPanel(const spacegen::Scene& scene,
                    const spacegen::MetalRenderer& renderer,
                    const FrameStats& stats,
                    bool* open) {
    ImGui::SetNextWindowSize(ImVec2(320.0f, 360.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(16.0f, 40.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Stats", open)) { ImGui::End(); return; }

    ImGui::TextColored(ImVec4(0.7f, 1.0f, 0.7f, 1.0f),
                       "%.1f fps  (%.2f ms/frame)",
                       stats.fps, stats.frameTimeMs);
    ImGui::Text("drawable:  %d x %d", stats.drawableW, stats.drawableH);

    ImGui::Separator();
    ImGui::TextDisabled("Scene");
    ImGui::Text("meshes:    %zu", renderer.meshCount());
    ImGui::Text("triangles: %zu", renderer.totalTriangles());
    ImGui::Text("schema:    v%d", scene.schemaVersion);
    if (!scene.sourceBlend.empty()) {
        ImGui::TextWrapped("source: %s", scene.sourceBlend.c_str());
    }

    ImGui::Separator();
    ImGui::TextDisabled("Camera");
    ImGui::Text("name:      %s", scene.camera.name.c_str());
    ImGui::Text("focal:     %.1f mm", scene.camera.focalLengthMM);
    ImGui::Text("FOV:       %.2f x %.2f deg",
                scene.camera.fovXRad * 57.2957795f,
                scene.camera.fovYRad * 57.2957795f);
    ImGui::Text("clip:      %.2f .. %.1f",
                scene.camera.clipStart, scene.camera.clipEnd);
    auto camPos = glm::vec3(scene.camera.world[3]);
    ImGui::Text("world pos: %+.2f %+.2f %+.2f", camPos.x, camPos.y, camPos.z);

    ImGui::End();
}

void drawInspector(spacegen::MetalRenderer& renderer, bool* open) {
    ImGui::SetNextWindowSize(ImVec2(360.0f, 480.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_FirstUseEver);
    ImGuiViewport* vp = ImGui::GetMainViewport();
    if (vp) {
        ImGui::SetNextWindowPos(
            ImVec2(vp->WorkPos.x + vp->WorkSize.x - 376.0f,
                   vp->WorkPos.y + 8.0f),
            ImGuiCond_FirstUseEver);
    }
    if (!ImGui::Begin("Inspector", open)) { ImGui::End(); return; }

    if (ImGui::CollapsingHeader("Structure material",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::ColorEdit3("Base color##mat",
                          &renderer.baseColor[0],
                          ImGuiColorEditFlags_PickerHueWheel
                          | ImGuiColorEditFlags_Float);
        ImGui::SliderFloat("Roughness##mat", &renderer.roughness, 0.04f, 1.0f);
        ImGui::SliderFloat("Metallic##mat",  &renderer.metallic,  0.0f, 1.0f);
        ImGui::SliderFloat("Ambient##mat",   &renderer.ambient,   0.0f, 0.4f);
    }

    if (ImGui::CollapsingHeader("Directional light",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Button("Reset direction")) {
            renderer.lightDirection = glm::vec3(0.4f, -0.6f, -0.6f);
        }
        ImGui::SliderFloat3("Direction##light",
                            &renderer.lightDirection[0], -1.0f, 1.0f);
        ImGui::ColorEdit3("Color##light",
                          &renderer.lightColor[0],
                          ImGuiColorEditFlags_PickerHueWheel
                          | ImGuiColorEditFlags_Float);
        ImGui::SliderFloat("Intensity##light",
                           &renderer.lightIntensity, 0.0f, 10.0f);
    }

    ImGui::End();
}

} // anon namespace

// ============================================================
// Workstation impl
// ============================================================

struct WorkstationState {
    bool showStats     = true;
    bool showInspector = true;
    bool showDemo      = false;
};

static WorkstationState g_state;

Workstation::Workstation(GLFWwindow* window,
                         MTL::Device* device,
                         MTL::PixelFormat /*colorFormat*/)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    styleSpaceGen();

    ImGui_ImplGlfw_InitForOther(window, true);
    ImGui_ImplMetal_Init((__bridge id<MTLDevice>)device);

    std::printf("[Workstation] ImGui %s ready (docking enabled)\n",
                 ImGui::GetVersion());
}

Workstation::~Workstation() {
    ImGui_ImplMetal_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
}

void Workstation::buildAndSubmit(MTL::CommandBuffer* cb,
                                  MTL::Texture* colorTarget,
                                  const Scene& scene,
                                  MetalRenderer& renderer,
                                  const FrameStats& stats)
{
    if (!cb || !colorTarget) return;

    // ---- 1. Build a render pass descriptor that LOADS the prior content
    //         (the structure pass already drew to this texture).
    MTL::RenderPassDescriptor* rpd =
        MTL::RenderPassDescriptor::renderPassDescriptor();
    auto* col = rpd->colorAttachments()->object(0);
    col->setTexture(colorTarget);
    col->setLoadAction(MTL::LoadActionLoad);
    col->setStoreAction(MTL::StoreActionStore);

    // ---- 2. ImGui frame begin
    ImGui_ImplMetal_NewFrame((__bridge MTLRenderPassDescriptor*)rpd);
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    // Dockspace over the central area (lets the rendered scene show through
    // because PassthruCentralNode keeps the central node click-through).
    ImGui::DockSpaceOverViewport(
        0,
        ImGui::GetMainViewport(),
        ImGuiDockNodeFlags_PassthruCentralNode);

    drawMainMenuBar(g_state.showStats, g_state.showInspector, g_state.showDemo);
    if (g_state.showStats)     drawStatsPanel(scene, renderer, stats,
                                              &g_state.showStats);
    if (g_state.showInspector) drawInspector(renderer, &g_state.showInspector);
    if (g_state.showDemo)      ImGui::ShowDemoWindow(&g_state.showDemo);

    ImGui::Render();

    // ---- 3. Encode draw data into a new pass on the same color target
    MTL::RenderCommandEncoder* enc = cb->renderCommandEncoder(rpd);
    if (enc) {
        ImGui_ImplMetal_RenderDrawData(
            ImGui::GetDrawData(),
            (__bridge id<MTLCommandBuffer>)cb,
            (__bridge id<MTLRenderCommandEncoder>)enc);
        enc->endEncoding();
    }
}

} // namespace spacegen::gui
