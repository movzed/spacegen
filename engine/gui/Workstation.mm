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
#include "../core/Layer.h"
#include "../core/StructureLayer.h"
#include "../core/BeamLayer.h"
#include "../core/DirectionalLightLayer.h"
#include "../core/AmbientLightLayer.h"
#include "../backends/metal/MetalRenderer.h"

#include <algorithm>
#include <cstdio>

namespace spacegen::gui {

namespace {

constexpr const char* kCompositionWindowName = "Composition";
constexpr const char* kStatsWindowName       = "Stats";
constexpr const char* kLayersWindowName      = "Layers";
constexpr const char* kModulatorsWindowName  = "Modulators";
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

    // Layout: 22% left -> Stats (top half) + Layers (bottom half).
    //         28% right -> Inspector.
    //         center   -> Composition (NoTabBar + NoDockingOverMe).
    ImGuiID dockCenter = dockspace_id;
    ImGuiID dockLeft   = ImGui::DockBuilderSplitNode(dockCenter, ImGuiDir_Left,
                                                      0.22f, nullptr, &dockCenter);
    ImGuiID dockRight  = ImGui::DockBuilderSplitNode(dockCenter, ImGuiDir_Right,
                                                      0.28f, nullptr, &dockCenter);
    // Split the left dock vertically: Stats on top, Layers below.
    ImGuiID dockLeftBottom = ImGui::DockBuilderSplitNode(dockLeft, ImGuiDir_Down,
                                                          0.55f, nullptr, &dockLeft);

    if (auto* node = ImGui::DockBuilderGetNode(dockCenter)) {
        node->LocalFlags |= ImGuiDockNodeFlags_NoTabBar
                          | ImGuiDockNodeFlags_NoDockingOverMe;
    }

    ImGui::DockBuilderDockWindow(kStatsWindowName,       dockLeft);
    ImGui::DockBuilderDockWindow(kLayersWindowName,      dockLeftBottom);
    ImGui::DockBuilderDockWindow(kModulatorsWindowName,  dockLeftBottom);
    ImGui::DockBuilderDockWindow(kInspectorWindowName,   dockRight);
    ImGui::DockBuilderDockWindow(kCompositionWindowName, dockCenter);
    ImGui::DockBuilderFinish(dockspace_id);
}

void drawMainMenuBar(bool& showStats, bool& showLayers,
                     bool& showModulators,
                     bool& showInspector, bool& showDemo) {
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
            ImGui::MenuItem(kStatsWindowName,      nullptr, &showStats);
            ImGui::MenuItem(kLayersWindowName,     nullptr, &showLayers);
            ImGui::MenuItem(kModulatorsWindowName, nullptr, &showModulators);
            ImGui::MenuItem(kInspectorWindowName,  nullptr, &showInspector);
            ImGui::Separator();
            ImGui::MenuItem("ImGui Demo", nullptr, &showDemo);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help")) {
            ImGui::MenuItem("SpaceGen M3-B", nullptr, false, false);
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

// Layer rack: list of layers in scene.bus with quick controls per row.
// Click a layer to select it (the Inspector below shows its drawInspector()).
void drawLayerRack(spacegen::Scene& scene,
                   uint32_t& selectedLayerId,
                   bool* open) {
    if (!ImGui::Begin(kLayersWindowName, open)) { ImGui::End(); return; }

    // Top toolbar: add buttons.
    if (ImGui::Button("+ Spot")) {
        auto* b = scene.bus.add<spacegen::BeamLayer>();
        char buf[64];
        std::snprintf(buf, sizeof(buf), "Spot %zu", scene.bus.layers.size());
        b->name         = buf;
        b->followCamera = true;
        b->origin       = glm::vec3(scene.camera.world[3]);
        b->panDeg       = 0.0f;
        b->tiltDeg      = 0.0f;
        selectedLayerId = b->id;
    }
    ImGui::SameLine();
    if (ImGui::Button("+ Directional")) {
        auto* d = scene.bus.add<spacegen::DirectionalLightLayer>();
        char buf[64];
        std::snprintf(buf, sizeof(buf), "Directional %zu",
                       scene.bus.layers.size());
        d->name = buf;
        selectedLayerId = d->id;
    }
    ImGui::SameLine();
    if (ImGui::Button("+ Ambient")) {
        auto* a = scene.bus.add<spacegen::AmbientLightLayer>();
        char buf[64];
        std::snprintf(buf, sizeof(buf), "Ambient %zu",
                       scene.bus.layers.size());
        a->name = buf;
        selectedLayerId = a->id;
    }
    ImGui::Separator();

    // Layer rows
    uint32_t toRemove = 0;
    for (size_t i = 0; i < scene.bus.layers.size(); ++i) {
        auto& layer = scene.bus.layers[i];
        if (!layer) continue;
        ImGui::PushID(static_cast<int>(layer->id));

        // Color tag
        ImVec4 tag(layer->colorTag.x, layer->colorTag.y, layer->colorTag.z, 1.0f);
        ImGui::ColorButton("##tag", tag,
                            ImGuiColorEditFlags_NoTooltip
                            | ImGuiColorEditFlags_NoPicker
                            | ImGuiColorEditFlags_NoBorder,
                            ImVec2(12.0f, 16.0f));
        ImGui::SameLine();

        // Enable toggle (state primary)
        bool enabled = layer->state == spacegen::LayerState::Enabled;
        if (ImGui::Checkbox("##en", &enabled)) {
            layer->state = enabled ? spacegen::LayerState::Enabled
                                    : spacegen::LayerState::Disabled;
        }
        ImGui::SameLine();

        // Selectable name (click selects). Selected row gets a clear visual
        // marker (arrow prefix + accent color) so the operator knows which
        // layer the Inspector is showing.
        bool isSelected = (layer->id == selectedLayerId);
        char label[96];
        std::snprintf(label, sizeof(label), "%s %s##sel",
                       isSelected ? ">" : " ",
                       layer->name.c_str());
        if (isSelected) {
            ImGui::PushStyleColor(ImGuiCol_Header,
                                   ImVec4(0.18f, 0.40f, 0.78f, 0.70f));
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered,
                                   ImVec4(0.22f, 0.46f, 0.86f, 0.85f));
            ImGui::PushStyleColor(ImGuiCol_Text,
                                   ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
        }
        if (ImGui::Selectable(label, isSelected,
                               ImGuiSelectableFlags_AllowDoubleClick,
                               ImVec2(0.0f, 0.0f))) {
            selectedLayerId = layer->id;
        }
        if (isSelected) {
            ImGui::PopStyleColor(3);
        }
        // Right-click on the row gives a context menu.
        if (ImGui::BeginPopupContextItem("##ctx")) {
            ImGui::TextDisabled("%s", layer->typeName());
            ImGui::Separator();
            if (ImGui::MenuItem("Remove")) {
                toRemove = layer->id;
            }
            ImGui::EndPopup();
        }

        // Compact blend + opacity (small row beneath)
        ImGui::Indent(20.0f);
        const char* blendNames[] = { "Normal", "Add" };
        int blendIdx = (layer->blendMode == spacegen::BlendMode::Add) ? 1 : 0;
        ImGui::SetNextItemWidth(80.0f);
        if (ImGui::Combo("##blend", &blendIdx, blendNames, IM_ARRAYSIZE(blendNames))) {
            layer->blendMode = (blendIdx == 1) ? spacegen::BlendMode::Add
                                                : spacegen::BlendMode::Normal;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110.0f);
        ImGui::SliderFloat("##op", &layer->opacity, 0.0f, 1.0f, "%.2f");
        ImGui::Unindent(20.0f);

        ImGui::PopID();
    }
    if (toRemove != 0) {
        // Don't remove the only StructureLayer if it's selected — for v1 the
        // StructureLayer is the only Structure type we have, so allow removal
        // (operator can re-add via a future menu). Keep simple.
        scene.bus.remove(toRemove);
        if (selectedLayerId == toRemove) selectedLayerId = 0;
    }
    ImGui::End();
}

// Modulators panel: global pool of N LFOs. Each can be bound from any
// modulatable parameter (currently spot pan / tilt / intensity) via the
// "Mod bank bindings" dropdowns in that layer's Inspector.
void drawModulators(spacegen::Scene& scene, bool* open) {
    if (!ImGui::Begin(kModulatorsWindowName, open)) { ImGui::End(); return; }
    ImGui::TextDisabled("Global LFOs (assign from any param's");
    ImGui::TextDisabled("'Mod bank bindings' dropdown)");
    ImGui::Separator();

    static const char* kWaveNames[] = { "Sine", "Triangle", "Square", "Saw" };

    for (int i = 0; i < spacegen::kModulatorBankSize; ++i) {
        auto& e = scene.modulators.entry(i);
        ImGui::PushID(i);

        // Header row: enable + slot number + name
        bool en = e.lfo.enabled;
        if (ImGui::Checkbox("##en", &en)) e.lfo.enabled = en;
        ImGui::SameLine();
        ImGui::Text("%d ▸ %s", i + 1, e.name.c_str());

        if (e.lfo.enabled) {
            ImGui::Indent(20.0f);
            int w = static_cast<int>(e.lfo.wave);
            ImGui::SetNextItemWidth(110.0f);
            if (ImGui::Combo("wave", &w, kWaveNames, 4)) {
                e.lfo.wave = static_cast<spacegen::LFO::Wave>(w);
            }
            ImGui::SliderFloat("freq (Hz)", &e.lfo.freqHz, 0.0f, 8.0f);
            ImGui::SliderFloat("phase",     &e.lfo.phase,  0.0f, 1.0f);
            ImGui::Unindent(20.0f);
        }
        ImGui::Separator();
        ImGui::PopID();
    }
    ImGui::End();
}

// Inspector: shows the selected layer's drawInspector(). If none selected,
// helpful empty state.
void drawInspector(spacegen::Scene& scene,
                   uint32_t selectedLayerId,
                   bool* open) {
    if (!ImGui::Begin(kInspectorWindowName, open)) { ImGui::End(); return; }

    spacegen::ILayer* sel = nullptr;
    for (auto& l : scene.bus.layers) {
        if (l && l->id == selectedLayerId) { sel = l.get(); break; }
    }
    if (!sel) {
        ImGui::TextDisabled("Select a layer in the Layers panel.");
        ImGui::End();
        return;
    }

    ImGui::Text("%s — %s", sel->name.c_str(), sel->typeName());
    char nameBuf[128];
    std::snprintf(nameBuf, sizeof(nameBuf), "%s", sel->name.c_str());
    if (ImGui::InputText("Name", nameBuf, sizeof(nameBuf))) {
        sel->name = nameBuf;
    }
    ImGui::Separator();
    sel->drawInspector();
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

CompositionTarget Workstation::beginFrame(int sceneOutputW,
                                            int sceneOutputH,
                                            const FrameStats& stats)
{
    currentStats_ = stats;

    // Offscreen size is FIXED to the scene's authored camera resolution.
    // It does NOT track the central dock node size — that's what would
    // deform the image when the operator resizes panels.
    ensureOffscreen(sceneOutputW, sceneOutputH);

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
                            Scene& scene,
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

    drawMainMenuBar(showStats_, showLayers_, showModulators_,
                     showInspector_, showDemo_);
    if (showStats_)      drawStatsPanel(scene, renderer,
                                        currentStats_, &showStats_);
    if (showLayers_)     drawLayerRack(scene, selectedLayerId_, &showLayers_);
    if (showModulators_) drawModulators(scene, &showModulators_);
    if (showInspector_)  drawInspector(scene, selectedLayerId_, &showInspector_);
    if (showDemo_)       ImGui::ShowDemoWindow(&showDemo_);

    // Selection bookkeeping:
    //  - If the selected id refers to a layer that no longer exists (removed
    //    via the Remove menu), clear the selection.
    //  - If nothing is selected and the bus has layers, pick a sensible
    //    default — the first non-Structure layer if available, otherwise
    //    the first layer.
    bool selectionExists = false;
    for (auto& l : scene.bus.layers) {
        if (l && l->id == selectedLayerId_) { selectionExists = true; break; }
    }
    if (!selectionExists) selectedLayerId_ = 0;
    if (selectedLayerId_ == 0 && !scene.bus.layers.empty()) {
        for (auto& l : scene.bus.layers) {
            if (l && std::string(l->typeName()) != "Structure") {
                selectedLayerId_ = l->id;
                break;
            }
        }
        if (selectedLayerId_ == 0 && scene.bus.layers.front()) {
            selectedLayerId_ = scene.bus.layers.front()->id;
        }
    }

    // ---- Composition central window: shows the offscreen texture ----
    // Use no-padding so the image fills the dock node edge-to-edge.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGuiWindowFlags compFlags = ImGuiWindowFlags_NoScrollbar
                                | ImGuiWindowFlags_NoScrollWithMouse
                                | ImGuiWindowFlags_NoCollapse
                                | ImGuiWindowFlags_NoTitleBar;
    if (ImGui::Begin(kCompositionWindowName, nullptr, compFlags)) {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        if (avail.x > 0.0f && avail.y > 0.0f
            && offscreen_ && offscreenW_ > 0 && offscreenH_ > 0)
        {
            // Compute fit-inside-with-aspect (letterbox / pillarbox). The
            // image is always rendered at the camera's authored resolution
            // (offscreen) and displayed scaled-to-fit without distortion.
            const float texAspect   = float(offscreenW_) / float(offscreenH_);
            const float availAspect = avail.x / avail.y;
            float drawW, drawH;
            if (availAspect > texAspect) {
                // wider available space than image -> fit by height (pillarbox)
                drawH = avail.y;
                drawW = drawH * texAspect;
            } else {
                // taller available space -> fit by width (letterbox)
                drawW = avail.x;
                drawH = drawW / texAspect;
            }
            // Center within the available region.
            ImVec2 cursor = ImGui::GetCursorPos();
            cursor.x += (avail.x - drawW) * 0.5f;
            cursor.y += (avail.y - drawH) * 0.5f;
            ImGui::SetCursorPos(cursor);

            ImTextureID texId =
                static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(offscreen_));
            ImGui::Image(texId, ImVec2(drawW, drawH));
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
