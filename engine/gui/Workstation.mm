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
#include "../core/SyphonInputLayer.h"
#include "../core/UvAtlas.h"
#include "../backends/metal/MetalRenderer.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <memory>
#include <thread>
#include <cstdio>

namespace spacegen::gui {

namespace {

constexpr const char* kCompositionWindowName = "Composition";
constexpr const char* kStatsWindowName       = "Stats";
constexpr const char* kLayersWindowName      = "Layers";
constexpr const char* kModulatorsWindowName  = "Modulators";
constexpr const char* kInspectorWindowName   = "Inspector";
constexpr const char* kUvAnalysisWindowName  = "UV Analysis";

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
    ImGui::DockBuilderDockWindow(kUvAnalysisWindowName,  dockLeftBottom);
    ImGui::DockBuilderDockWindow(kInspectorWindowName,   dockRight);
    ImGui::DockBuilderDockWindow(kCompositionWindowName, dockCenter);
    ImGui::DockBuilderFinish(dockspace_id);
}

void drawMainMenuBar(bool& showStats, bool& showLayers,
                     bool& showModulators,
                     bool& showInspector, bool& showDemo,
                     bool& showUvAnalysis) {
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
            ImGui::MenuItem(kUvAnalysisWindowName, nullptr, &showUvAnalysis);
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
    ImGui::SameLine();
    if (ImGui::Button("+ Syphon")) {
        auto* s = scene.bus.add<spacegen::SyphonInputLayer>();
        char buf[64];
        std::snprintf(buf, sizeof(buf), "Syphon %zu",
                       scene.bus.layers.size());
        s->name = buf;
        selectedLayerId = s->id;
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

// ---- UV Analysis panel ----------------------------------------------------
// Per-mesh UV diagnostics + ability to regenerate the SyphonUV layer with
// xatlas (industry-standard automatic atlas generator). The regeneration is
// blocking (the UI freezes for the duration of xatlas's run; expect 30s to
// several minutes on a multi-million-triangle mesh). The result is cached
// to disk as uv1_atlas.bin next to scene.json. On the next launch, the
// engine loads that cache automatically.
//
// We intentionally do NOT swap GPU buffers at runtime — that would require
// rebuilding the index/vertex buffers mid-frame, and xatlas can change the
// vertex count (it splits verts at chart seams). The user restarts the
// binary to apply.

// Persistent state of the panel between frames.
//
// xatlas runs on a worker thread so the UI stays responsive. The worker
// communicates with the main thread through atomics (progress %, stage,
// done flag, cancel request). Once the worker reports done, the main
// thread joins, copies the result, persists the cache, and resets.
//
// All atomics are owned by this struct and live for the lifetime of the
// Workstation; we never delete them while a worker holds a pointer.
struct UvAnalysisState {
    // ---- One-shot UV coverage stats ----
    bool        coverageComputed = false;
    int         totalTris        = 0;
    int         uv0DegenTris     = 0;
    int         uv1DegenTris     = 0;

    // ---- xatlas worker state ----
    enum class Status : int { Idle = 0, Running = 1, Done = 2 };
    std::atomic<int>    status{static_cast<int>(Status::Idle)};
    std::atomic<int>    progressPct{0};
    std::atomic<int>    stageId{0};    // 0 idle, 1 add, 2 charts, 3 pack, 4 build, 5 save
    std::atomic<bool>   shouldCancel{false};
    std::atomic<double> startTimeSec{0.0};

    std::unique_ptr<std::thread>            worker;
    std::unique_ptr<spacegen::UvAtlasResult> result;   // produced by worker

    // ---- Last-run status (rendered post-completion) ----
    bool        lastOk          = false;
    std::string lastMsg;
    int         lastChartCount  = 0;
    int         lastAtlasW      = 0;
    int         lastAtlasH      = 0;
    double      lastRegenSec    = 0.0;

    // Cleanly stop and join any running worker. Called when the panel /
    // app shuts down so we don't leave a detached thread behind.
    void joinWorker() {
        if (worker) {
            shouldCancel.store(true);
            if (worker->joinable()) worker->join();
            worker.reset();
        }
        status.store(static_cast<int>(Status::Idle));
        progressPct.store(0);
        stageId.store(0);
        shouldCancel.store(false);
    }

    ~UvAnalysisState() { joinWorker(); }
};

static UvAnalysisState gUvState;

// Pre-cut control state, captured by the worker thread at button-click time.
// Kept here (not in UvAnalysisState) because they are pure inputs that the
// panel reads/writes, not part of the worker's lifecycle state machine.
static bool  gUvSharpPreCut   = true;
static float gUvSharpAngleDeg = 35.0f;

// xatlas progress trampoline. Runs on the worker thread; writes to atomics
// that the main thread reads each frame. Returns false when the operator
// has clicked Cancel — xatlas then aborts the current phase.
static bool uvWorkerProgress(int pct, const char* stageName, void* user) {
    auto* s = static_cast<UvAnalysisState*>(user);
    if (!s) return true;
    int stageId = 0;
    if (stageName) {
        if (std::strstr(stageName, "Validating"))   stageId = 1;
        else if (std::strstr(stageName, "Detecting")) stageId = 2;
        else if (std::strstr(stageName, "Packing"))   stageId = 3;
        else if (std::strstr(stageName, "Building"))  stageId = 4;
    }
    s->stageId.store(stageId);
    s->progressPct.store(pct);
    return !s->shouldCancel.load();
}

// Walk every triangle of `m` and compute how many have UV-degenerate triangles
// in uv0 and uv1. A degenerate triangle is one whose three vertices share the
// same UV coordinates within `eps` — i.e. the face was never unwrapped.
static void recomputeUvCoverage(const spacegen::MeshData& m, UvAnalysisState& s) {
    s.coverageComputed = true;
    s.totalTris    = static_cast<int>(m.indices.size() / 3);
    s.uv0DegenTris = 0;
    s.uv1DegenTris = 0;
    if (s.totalTris == 0) return;

    const bool haveUv0 = !m.uvs.empty()  && m.uvs.size()  == m.positions.size();
    const bool haveUv1 = !m.uvs1.empty() && m.uvs1.size() == m.positions.size();
    const float eps = 1e-6f;

    auto isDegen = [eps](const std::vector<glm::vec2>& uvs,
                          uint32_t a, uint32_t b, uint32_t c) {
        if (a >= uvs.size() || b >= uvs.size() || c >= uvs.size()) return true;
        const glm::vec2& va = uvs[a];
        const glm::vec2& vb = uvs[b];
        const glm::vec2& vc = uvs[c];
        float du = std::max({va.x, vb.x, vc.x}) - std::min({va.x, vb.x, vc.x});
        float dv = std::max({va.y, vb.y, vc.y}) - std::min({va.y, vb.y, vc.y});
        return (du + dv) < eps;
    };

    for (size_t i = 0; i + 2 < m.indices.size(); i += 3) {
        uint32_t a = m.indices[i + 0];
        uint32_t b = m.indices[i + 1];
        uint32_t c = m.indices[i + 2];
        if (haveUv0 && isDegen(m.uvs,  a, b, c)) ++s.uv0DegenTris;
        if (haveUv1 && isDegen(m.uvs1, a, b, c)) ++s.uv1DegenTris;
    }
    if (!haveUv0) s.uv0DegenTris = s.totalTris;
    if (!haveUv1) s.uv1DegenTris = s.totalTris;
}

void drawUvAnalysisPanel(spacegen::Scene& scene,
                          spacegen::MetalRenderer& renderer,
                          bool* open) {
    if (!ImGui::Begin(kUvAnalysisWindowName, open)) { ImGui::End(); return; }

    if (scene.meshes.empty()) {
        ImGui::TextDisabled("No meshes loaded.");
        ImGui::End();
        return;
    }
    spacegen::MeshData& m = scene.meshes[0];

    // Header
    ImGui::Text("Mesh: %s", m.name.c_str());
    ImGui::Text("Verts: %zu   Tris: %zu",
                 m.positions.size(), m.indices.size() / 3);

    // Stats: button to compute (potentially slow on 7M-tri meshes), or auto
    // on first show.
    if (!gUvState.coverageComputed) {
        recomputeUvCoverage(m, gUvState);
    }
    ImGui::Separator();
    ImGui::TextDisabled("UV coverage (per-triangle degeneracy)");
    auto bar = [](const char* label, int degen, int total) {
        float frac = total > 0
            ? 1.0f - static_cast<float>(degen) / static_cast<float>(total)
            : 0.0f;
        char overlay[64];
        std::snprintf(overlay, sizeof(overlay), "%s: %.1f%% valid",
                       label, frac * 100.0f);
        ImVec4 col = frac > 0.95f
            ? ImVec4(0.30f, 0.85f, 0.40f, 1.0f)
            : (frac > 0.5f
                ? ImVec4(0.95f, 0.85f, 0.30f, 1.0f)
                : ImVec4(0.95f, 0.40f, 0.30f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, col);
        ImGui::ProgressBar(frac, ImVec2(-FLT_MIN, 0), overlay);
        ImGui::PopStyleColor();
    };
    bar("UV0 (materials)",      gUvState.uv0DegenTris, gUvState.totalTris);
    bar("UV1 (SyphonUV)",       gUvState.uv1DegenTris, gUvState.totalTris);
    if (ImGui::SmallButton("Recompute coverage")) {
        recomputeUvCoverage(m, gUvState);
    }

    // ---- Active UV layer indicator ----
    ImGui::Separator();
    ImVec4 activeCol = scene.atlasApplied
        ? ImVec4(0.30f, 0.85f, 0.40f, 1.0f)
        : ImVec4(0.50f, 0.75f, 0.95f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, activeCol);
    ImGui::Text("Atlas applied: %s",
        scene.atlasApplied ? "YES (UV1 = xatlas)"
                            : "NO  (UV1 = same as UV0)");
    ImGui::PopStyleColor();
    ImGui::TextDisabled("In SyphonInputLayer, 'Use generated atlas UVs'");
    ImGui::TextDisabled("samples UV1 — meaningful only when YES above.");

    // ---- Cache file status ----
    ImGui::Separator();
    std::filesystem::path cachePath =
        std::filesystem::path(scene.folderPath) / "uv1_atlas.bin";
    bool cacheExists = std::filesystem::exists(cachePath);
    ImGui::TextDisabled("Cache: %s",
        cacheExists ? cachePath.string().c_str()
                    : "(none yet — generate to create)");
    if (cacheExists) {
        auto sizeBytes = std::filesystem::file_size(cachePath);
        ImGui::TextDisabled("       %.1f MB on disk", sizeBytes / (1024.0 * 1024.0));

        // Hot-reload: load the cache and swap the GPU mesh in place.
        // Avoids the restart loop the operator had to do before.
        ImGui::BeginDisabled(scene.atlasApplied);
        if (ImGui::Button("Apply atlas UVs now (hot-reload, no restart)",
                           ImVec2(-FLT_MIN, 32))) {
            uint64_t fp = spacegen::meshFingerprint(m);
            spacegen::UvAtlasResult cached;
            if (spacegen::loadAtlasCache(cachePath.string(), fp, cached)) {
                m.positions = std::move(cached.positions);
                m.normals   = std::move(cached.normals);
                m.uvs       = std::move(cached.uvs0);
                m.uvs1      = std::move(cached.uvs1);
                m.indices   = std::move(cached.indices);
                if (renderer.reloadMesh(0, m)) {
                    scene.atlasApplied = true;
                    recomputeUvCoverage(m, gUvState);
                    gUvState.lastOk  = true;
                    gUvState.lastMsg = "Atlas hot-loaded. UV1 now points to xatlas.";
                } else {
                    gUvState.lastOk  = false;
                    gUvState.lastMsg = "Atlas loaded from disk but GPU upload failed.";
                }
            } else {
                gUvState.lastOk  = false;
                gUvState.lastMsg = "Cache exists but couldn't be loaded "
                                    "(fingerprint mismatch or corrupt file).";
            }
        }
        ImGui::EndDisabled();
        if (scene.atlasApplied) {
            ImGui::TextDisabled("Atlas already active in memory.");
        }
    }

    // ---- Tier 2: Stretch heatmap diagnostic ----
    ImGui::Separator();
    // Find the StructureLayer in the bus to mutate its heatmap flags.
    spacegen::StructureLayer* slayer = nullptr;
    for (auto& l : scene.bus.layers) {
        if (auto* sl = dynamic_cast<spacegen::StructureLayer*>(l.get())) {
            slayer = sl; break;
        }
    }
    if (slayer) {
        ImGui::TextDisabled("Stretch heatmap diagnostic");
        ImGui::Checkbox("Show stretch heatmap##sh", &slayer->showStretchHeatmap);
        if (slayer->showStretchHeatmap) {
            static const char* kMetricNames[] = {
                "Stretch ratio (ideal 1.0)",
                "Symmetric Dirichlet energy (ideal 4.0)",
            };
            ImGui::Combo("Metric##shm", &slayer->stretchMetric,
                          kMetricNames, 2);
            static const char* kUVNames[] = {
                "UV0 (Blender PRT_UVW)",
                "UV1 (xatlas atlas)",
            };
            ImGui::Combo("Measure##shu", &slayer->stretchUV,
                          kUVNames, 2);
            // Legend strip: white → cyan → green → red gradient
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2  p0 = ImGui::GetCursorScreenPos();
            float   w  = ImGui::GetContentRegionAvail().x - 8.0f;
            float   h  = 14.0f;
            const ImU32 cols[] = {
                IM_COL32(255, 255, 255, 255),
                IM_COL32( 51, 217, 242, 255),
                IM_COL32( 77, 217,  77, 255),
                IM_COL32(242,  77,  51, 255),
            };
            float seg = w / 3.0f;
            for (int i = 0; i < 3; ++i) {
                dl->AddRectFilledMultiColor(
                    ImVec2(p0.x + i * seg,       p0.y),
                    ImVec2(p0.x + (i + 1) * seg, p0.y + h),
                    cols[i], cols[i + 1], cols[i + 1], cols[i]);
            }
            ImGui::Dummy(ImVec2(w, h));
            ImGui::TextDisabled("white = ideal     cyan ~ minor     "
                                 "green ~ moderate     red = broken");
            ImGui::TextDisabled("(Lighting bypassed in heatmap mode.)");
        }
    }

    // ---- xatlas regenerate ----
    ImGui::Separator();
    ImGui::TextDisabled("Regenerate SyphonUV with xatlas");
    ImGui::TextWrapped("xatlas with Tier-1 enhancements: dihedral pre-cut "
                        "(RizomUV Sharp Edges algorithm) forces chart "
                        "boundaries onto geometric creases — invisible "
                        "in projection — followed by big-chart packing.");

    // Tier-1 controls: Sharp Edges pre-cut
    ImGui::Checkbox("Sharp Edges pre-cut##sharp", &gUvSharpPreCut);
    if (gUvSharpPreCut) {
        ImGui::SliderFloat("Dihedral threshold (°)##ang",
                            &gUvSharpAngleDeg, 5.0f, 90.0f, "%.1f°");
        ImGui::TextDisabled("Edges with dihedral > threshold become forced");
        ImGui::TextDisabled("chart boundaries. Lower = more seams on creases.");
    } else {
        ImGui::TextDisabled("Off: xatlas picks seams freely (may cut mid-surface).");
    }

    // Decide what to render based on the worker status.
    int curStatus = gUvState.status.load();
    using S = UvAnalysisState::Status;

    if (curStatus == static_cast<int>(S::Idle)) {
        // ---- Idle: offer the Generate button. ----
        if (ImGui::Button("Generate UV atlas with xatlas",
                           ImVec2(-FLT_MIN, 36))) {
            // Capture all needed inputs by value into the worker. We pass
            // the MeshData by const reference because Scene outlives the
            // worker (Scene is owned by main()'s stack and we join the
            // worker before shutdown).
            gUvState.status.store(static_cast<int>(S::Running));
            gUvState.progressPct.store(0);
            gUvState.stageId.store(0);
            gUvState.shouldCancel.store(false);
            gUvState.startTimeSec.store(
                std::chrono::duration<double>(
                    std::chrono::steady_clock::now().time_since_epoch()
                ).count());
            gUvState.result.reset();

            const spacegen::MeshData* meshPtr = &m;
            std::string cacheStr = cachePath.string();

            gUvState.worker = std::make_unique<std::thread>(
                [meshPtr, cacheStr]() {
                    // Top-tier VJ mapping options. Defaults in xatlas are
                    // tuned for LIGHTMAPS (many small charts, tight
                    // packing) — wrong for live video. We:
                    //   1. Pre-cut the mesh on sharp geometric edges
                    //      (RizomUV Sharp Edges algorithm). xatlas then
                    //      lands every chart boundary on a natural crease
                    //      of the geometry, invisible in projection.
                    //   2. Bias xatlas towards FEW LARGE charts so the
                    //      video flows continuously across coplanar
                    //      regions. normalDeviationW high, roundness/
                    //      straightness = 0.
                    spacegen::UvAtlasOptions opts;
                    opts.maxAtlasSize       = 4096;
                    opts.bruteForcePack     = true;
                    opts.bilinearPadding    = true;
                    opts.normalDeviationW   = 10.0f;
                    opts.roundnessW         = 0.0f;
                    opts.straightnessW      = 0.0f;
                    opts.normalSeamW        = 1.0f;
                    opts.textureSeamW       = 0.0f;
                    opts.preCutSharpEdges   = gUvSharpPreCut;
                    opts.sharpEdgeAngleDeg  = gUvSharpAngleDeg;

                    auto t0 = std::chrono::steady_clock::now();
                    // Sharp-edge pre-pass on a local copy of the mesh.
                    // Result is consumed by xatlas without touching the
                    // GPU-resident mesh until the worker finishes.
                    spacegen::MeshData working = *meshPtr;
                    if (opts.preCutSharpEdges) {
                        gUvState.stageId.store(0);
                        working = spacegen::preCutOnSharpEdges(
                            working, opts.sharpEdgeAngleDeg);
                    }
                    auto res = std::make_unique<spacegen::UvAtlasResult>(
                        spacegen::generateAtlas(working, opts,
                                                uvWorkerProgress, &gUvState));
                    auto t1 = std::chrono::steady_clock::now();
                    double secs =
                        std::chrono::duration<double>(t1 - t0).count();
                    gUvState.lastRegenSec = secs;

                    // Persist if successful. This is the long write of the
                    // cache file; we keep it on the worker thread too so
                    // the main UI never blocks.
                    if (res->ok && !gUvState.shouldCancel.load()) {
                        gUvState.stageId.store(5);
                        gUvState.progressPct.store(0);
                        uint64_t fp = spacegen::meshFingerprint(*meshPtr);
                        bool saved = spacegen::saveAtlasCache(cacheStr, fp, *res);
                        gUvState.lastChartCount = res->chartCount;
                        gUvState.lastAtlasW     = res->atlasWidth;
                        gUvState.lastAtlasH     = res->atlasHeight;
                        gUvState.lastOk         = saved;
                        gUvState.lastMsg        = saved
                            ? "Cache saved. Restart the app to apply the new UVs."
                            : "Atlas generated but writing cache failed.";
                        gUvState.progressPct.store(100);
                    } else if (gUvState.shouldCancel.load()) {
                        gUvState.lastOk  = false;
                        gUvState.lastMsg = "Cancelled by operator.";
                    } else {
                        gUvState.lastOk  = false;
                        gUvState.lastMsg = std::string("xatlas failed: ")
                                          + res->error;
                    }
                    gUvState.result = std::move(res);
                    gUvState.status.store(static_cast<int>(S::Done));
                });
        }

        // Show the previous run's result, if any.
        if (!gUvState.lastMsg.empty()) {
            ImVec4 col = gUvState.lastOk
                ? ImVec4(0.30f, 0.85f, 0.40f, 1.0f)
                : ImVec4(0.95f, 0.40f, 0.30f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, col);
            ImGui::TextWrapped("%s", gUvState.lastMsg.c_str());
            ImGui::PopStyleColor();
            if (gUvState.lastOk) {
                ImGui::TextDisabled("Charts: %d   Atlas: %dx%d   Time: %.1fs",
                                     gUvState.lastChartCount,
                                     gUvState.lastAtlasW, gUvState.lastAtlasH,
                                     gUvState.lastRegenSec);
            }
        }
    } else if (curStatus == static_cast<int>(S::Running)) {
        // ---- Running: live progress bar + cancel ----
        const char* stageNames[] = {
            "Initializing...",
            "Validating geometry",
            "Detecting charts",
            "Packing atlas (brute force)",
            "Building output mesh",
            "Saving cache to disk",
        };
        int stage = gUvState.stageId.load();
        if (stage < 0 || stage > 5) stage = 0;
        int pct = gUvState.progressPct.load();

        double startTs = gUvState.startTimeSec.load();
        double nowTs   = std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count();
        double elapsed = nowTs - startTs;

        ImGui::TextUnformatted("Running xatlas on worker thread...");
        ImGui::TextDisabled("Stage %d/5: %s", stage, stageNames[stage]);

        char overlay[96];
        std::snprintf(overlay, sizeof(overlay), "%d%%  (%.1fs)",
                       pct, elapsed);
        ImGui::ProgressBar(pct / 100.0f, ImVec2(-FLT_MIN, 0), overlay);

        // Sub-progress hint for the stages without an internal pct
        // (xatlas reports per-phase, not global, so the bar resets per
        // stage — make that clear).
        ImGui::TextDisabled("xatlas reports progress per-stage; the bar");
        ImGui::TextDisabled("resets between phases. Total run typically");
        ImGui::TextDisabled("dominated by 'Detecting charts' on dense meshes.");

        if (ImGui::Button("Cancel", ImVec2(-FLT_MIN, 28))) {
            gUvState.shouldCancel.store(true);
        }
        if (gUvState.shouldCancel.load()) {
            ImGui::TextDisabled("Cancellation requested — waiting for");
            ImGui::TextDisabled("xatlas to reach a safe boundary...");
        }
    } else if (curStatus == static_cast<int>(S::Done)) {
        // ---- Done: join, then auto-hot-load the result so the operator
        // sees the new UVs immediately, no restart, no extra click. ----
        if (gUvState.worker && gUvState.worker->joinable()) {
            gUvState.worker->join();
        }
        gUvState.worker.reset();

        if (gUvState.result && gUvState.result->ok && gUvState.lastOk) {
            // Hot-swap mesh in place using the freshly generated result.
            spacegen::UvAtlasResult& r = *gUvState.result;
            m.positions = std::move(r.positions);
            m.normals   = std::move(r.normals);
            m.uvs       = std::move(r.uvs0);
            m.uvs1      = std::move(r.uvs1);
            m.indices   = std::move(r.indices);
            if (renderer.reloadMesh(0, m)) {
                scene.atlasApplied = true;
                gUvState.lastMsg = "Atlas generated AND applied. UV1 is now "
                                    "the xatlas conformal mapping — switch "
                                    "the Syphon layer's 'Use atlas' toggle "
                                    "to see it.";
            } else {
                gUvState.lastMsg += " (GPU upload failed; cache is on disk.)";
                gUvState.lastOk = false;
            }
            recomputeUvCoverage(m, gUvState);
        }
        gUvState.result.reset();

        gUvState.status.store(static_cast<int>(S::Idle));
        gUvState.progressPct.store(0);
        gUvState.stageId.store(0);
        gUvState.shouldCancel.store(false);
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
    // Stop any running xatlas worker BEFORE the Scene's MeshData goes out
    // of scope in main(). The worker captures a pointer to the mesh; if
    // we let it run past Scene destruction, the dangling pointer is UB.
    gUvState.joinWorker();
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
                     showInspector_, showDemo_, showUvAnalysis_);
    if (showStats_)      drawStatsPanel(scene, renderer,
                                        currentStats_, &showStats_);
    if (showLayers_)     drawLayerRack(scene, selectedLayerId_, &showLayers_);
    if (showModulators_) drawModulators(scene, &showModulators_);
    if (showUvAnalysis_) drawUvAnalysisPanel(scene, renderer, &showUvAnalysis_);
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
