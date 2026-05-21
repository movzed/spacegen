// Workstation.mm — ObjC++ implementation of the ImGui workstation.
// Bridges Metal-cpp pointers to the ImGui Metal backend's ObjC ids.

#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_metal.h"

#include "Workstation.h"
#include "../core/Scene.h"
#include "../backends/metal/MetalRenderer.h"

#include <algorithm>
#include <cstdio>

namespace spacegen::gui {

namespace {

constexpr const char* kCompositionWindowName = "Composition";
constexpr const char* kStatsWindowName       = "Stats";
constexpr const char* kInspectorWindowName   = "Inspector";

void styleSpaceGen() {
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding    = 4.0f;
    s.FrameRounding     = 3.0f;
    s.GrabRounding      = 3.0f;
    s.TabRounding       = 3.0f;
    s.WindowPadding     = ImVec2(10.0f, 10.0f);
    s.FramePadding      = ImVec2(8.0f, 4.0f);
    s.ItemSpacing       = ImVec2(8.0f, 6.0f);
    s.WindowBorderSize  = 1.0f;
    s.FrameBorderSize   = 0.0f;
}

// Build the default dockspace layout: Stats left | Composition center | Inspector right.
// Called once on first frame (after the dockspace itself has been created).
void buildDefaultLayout(ImGuiID dockspace_id) {
    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);

    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::DockBuilderSetNodeSize(dockspace_id, vp->WorkSize);

    // Split: 22% left → Stats. Remainder of 78% then split: 28% of THAT → Inspector right.
    ImGuiID dockLeft = 0, dockRight = 0, dockCenter = dockspace_id;
    dockLeft   = ImGui::DockBuilderSplitNode(dockCenter, ImGuiDir_Left,
                                              0.22f, nullptr, &dockCenter);
    dockRight  = ImGui::DockBuilderSplitNode(dockCenter, ImGuiDir_Right,
                                              0.28f, nullptr, &dockCenter);

    // Mark central node so the user can't accidentally dock other windows
    // into it (the composition Image always lives there).
    if (auto* node = ImGui::DockBuilderGetNode(dockCenter)) {
        node->LocalFlags |= ImGuiDockNodeFlags_NoTabBar
                          | ImGuiDockNodeFlags_NoDockingOverMe;
    }

    ImGui::DockBuilderDockWindow(kStatsWindowName,       dockLeft);
    ImGui::DockBuilderDockWindow(kInspectorWindowName,   dockRight);
    ImGui::DockBuilderDockWindow(kCompositionWindowName, dockCenter);
    ImGui::DockBuilderFinish(dockspace_id);
}

void drawMainMenuBar(bool& showStats, bool& showInspector, bool& showDemo) {
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            ImGui::MenuItem("New scene...",   nullptr, false, false);
            ImGui::MenuItem("Open scene...",  nullptr, false, false);
            ImGui::Separator();
            ImGui::MenuItem("Save preset...", nullptr, false, false);
            ImGui::Separator();
            ImGui::MenuItem("Quit (ESC)",     nullptr, false, false);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            ImGui::MenuItem(kStatsWindowName,     nullptr, &showStats);
            ImGui::MenuItem(kInspectorWindowName, nullptr, &showInspector);
            ImGui::Separator();
            ImGui::MenuItem("ImGui Demo", nullptr, &showDemo);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help")) {
            ImGui::MenuItem("SpaceGen M2-C3", nullptr, false, false);
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
    if (!ImGui::Begin(kStatsWindowName, open)) { ImGui::End(); return; }

    ImGui::TextColored(ImVec4(0.7f, 1.0f, 0.7f, 1.0f),
                       "%.1f fps  (%.2f ms)", stats.fps, stats.frameTimeMs);
    ImGui::Text("drawable:  %d x %d", stats.drawableW, stats.drawableH);

    ImGui::Separator();
    ImGui::TextDisabled("Scene");
    ImGui::Text("meshes:    %zu",  renderer.meshCount());
    ImGui::Text("triangles: %zu",  renderer.totalTriangles());
    ImGui::Text("schema:    v%d",  scene.schemaVersion);
    if (!scene.sourceBlend.empty()) {
        ImGui::TextWrapped("source: %s", scene.sourceBlend.c_str());
    }

    ImGui::Separator();
    ImGui::TextDisabled("Camera");
    ImGui::Text("name:      %s",       scene.camera.name.c_str());
    ImGui::Text("focal:     %.1f mm",  scene.camera.focalLengthMM);
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
    if (!ImGui::Begin(kInspectorWindowName, open)) { ImGui::End(); return; }

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

Workstation::Workstation(GLFWwindow* window,
                          MTL::Device* device,
                          MTL::PixelFormat colorFormat)
    : device_(device)
    , offscreenFormat_(colorFormat)
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
    releaseOffscreen();
    ImGui_ImplMetal_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
}

void Workstation::releaseOffscreen() {
    if (offscreen_) {
        offscreen_->release();
        offscreen_ = nullptr;
    }
    offscreenW_ = offscreenH_ = 0;
}

void Workstation::ensureOffscreen(int widthPx, int heightPx) {
    widthPx  = std::max(widthPx,  8);
    heightPx = std::max(heightPx, 8);
    if (offscreen_
        && widthPx  == offscreenW_
        && heightPx == offscreenH_) {
        return;
    }
    releaseOffscreen();
    MTL::TextureDescriptor* td = MTL::TextureDescriptor::texture2DDescriptor(
        offscreenFormat_, widthPx, heightPx, false);
    td->setStorageMode(MTL::StorageModePrivate);
    td->setUsage(MTL::TextureUsageRenderTarget
                  | MTL::TextureUsageShaderRead);
    offscreen_  = device_->newTexture(td);
    offscreenW_ = widthPx;
    offscreenH_ = heightPx;
}

CompositionTarget Workstation::beginFrame(int swapchainW,
                                            int swapchainH,
                                            const FrameStats& stats)
{
    currentStats_ = stats;

    // First-frame size estimate: 50% of viewport at backing scale. After
    // the first frame this is overwritten with the actual measured size.
    if (cachedCompW_ == 0 || cachedCompH_ == 0) {
        // ImGui hasn't started a frame yet, so io.DisplayFramebufferScale
        // may be stale. Use swapchain / NOT-yet-known logical-window ratio:
        // assume 2x retina if drawableW > 0 and reasonable. Safe default = 2.
        float sx = 2.0f;
        float sy = 2.0f;
        if (swapchainW > 0 && swapchainH > 0) {
            cachedCompW_ = static_cast<int>(swapchainW * 0.50f);
            cachedCompH_ = static_cast<int>(swapchainH * 0.95f);
        } else {
            cachedCompW_ = 800;
            cachedCompH_ = 600;
        }
        (void)sx; (void)sy;
    }
    ensureOffscreen(cachedCompW_, cachedCompH_);

    // RPD passed to ImGui_ImplMetal_NewFrame must have a real texture so the
    // backend can read pixelFormat + sampleCount for pipeline state setup.
    // Our offscreen has the same format as the swapchain (configured by the
    // constructor) so the same ImGui pipeline state works for both.
    MTL::RenderPassDescriptor* rpd =
        MTL::RenderPassDescriptor::renderPassDescriptor();
    auto* col = rpd->colorAttachments()->object(0);
    col->setTexture(offscreen_);
    col->setLoadAction(MTL::LoadActionLoad);
    col->setStoreAction(MTL::StoreActionStore);

    ImGui_ImplMetal_NewFrame((__bridge MTLRenderPassDescriptor*)rpd);
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    return {offscreen_, offscreenW_, offscreenH_};
}

void Workstation::endFrame(MTL::CommandBuffer* cb,
                            MTL::Texture* swapchainTarget,
                            const Scene& scene,
                            MetalRenderer& renderer)
{
    if (!cb || !swapchainTarget) return;

    // ---- Dockspace ----
    ImGuiID dockspace_id = ImGui::GetID("SpaceGenDockSpace");
    ImGui::DockSpaceOverViewport(dockspace_id,
                                  ImGui::GetMainViewport(),
                                  ImGuiDockNodeFlags_None);
    if (!layoutBuilt_) {
        buildDefaultLayout(dockspace_id);
        layoutBuilt_ = true;
    }

    drawMainMenuBar(showStats_, showInspector_, showDemo_);
    if (showStats_)     drawStatsPanel(scene, renderer,
                                       currentStats_, &showStats_);
    if (showInspector_) drawInspector(renderer, &showInspector_);
    if (showDemo_)      ImGui::ShowDemoWindow(&showDemo_);

    // ---- Composition central window: shows the offscreen texture ----
    // Use no-padding so the image fills the dock node edge-to-edge.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGuiWindowFlags compFlags = ImGuiWindowFlags_NoScrollbar
                                | ImGuiWindowFlags_NoScrollWithMouse
                                | ImGuiWindowFlags_NoCollapse
                                | ImGuiWindowFlags_NoTitleBar;
    if (ImGui::Begin(kCompositionWindowName, nullptr, compFlags)) {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        if (avail.x > 0.0f && avail.y > 0.0f) {
            ImGuiIO& io = ImGui::GetIO();
            float sx = io.DisplayFramebufferScale.x > 0 ? io.DisplayFramebufferScale.x : 1.0f;
            float sy = io.DisplayFramebufferScale.y > 0 ? io.DisplayFramebufferScale.y : 1.0f;
            // Cache for NEXT frame's offscreen allocation.
            cachedCompW_ = static_cast<int>(avail.x * sx);
            cachedCompH_ = static_cast<int>(avail.y * sy);

            // Display the texture we rendered into during beginFrame's caller.
            // ImTextureID on Metal backend is reinterpreted as id<MTLTexture>;
            // Metal-cpp pointers are bit-compatible.
            ImTextureID texId =
                static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(offscreen_));
            ImGui::Image(texId, avail);
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();

    // ---- Render ImGui draw data into the swapchain ----
    ImGui::Render();

    MTL::RenderPassDescriptor* rpd =
        MTL::RenderPassDescriptor::renderPassDescriptor();
    auto* col = rpd->colorAttachments()->object(0);
    col->setTexture(swapchainTarget);
    col->setLoadAction(MTL::LoadActionClear);
    col->setStoreAction(MTL::StoreActionStore);
    // Background behind any panel that doesn't fully tile the swapchain.
    // With docking the entire window is covered, but during the very first
    // frames before layout settles this fills any gap.
    col->setClearColor(MTL::ClearColor(0.05, 0.06, 0.08, 1.0));

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
