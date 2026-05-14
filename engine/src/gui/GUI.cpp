#include "GUI.h"
#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"
static const char* kBlendNames[] = { "Additive", "Normal", "Screen", "Multiply" };

bool GUI::init(GLFWwindow* window) {
    if (m_initialized) return true;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename  = "spacegen_layout.ini";

    // Dark style with slight adjustments for a pro AV aesthetic
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding    = 4.0f;
    s.FrameRounding     = 3.0f;
    s.GrabRounding      = 3.0f;
    s.ItemSpacing       = {6.0f, 4.0f};
    s.WindowPadding     = {8.0f, 8.0f};
    s.Colors[ImGuiCol_WindowBg]     = ImVec4(0.10f, 0.10f, 0.12f, 0.96f);
    s.Colors[ImGuiCol_Header]       = ImVec4(0.20f, 0.20f, 0.28f, 1.00f);
    s.Colors[ImGuiCol_HeaderHovered]= ImVec4(0.28f, 0.28f, 0.40f, 1.00f);
    s.Colors[ImGuiCol_SliderGrab]   = ImVec4(0.40f, 0.60f, 1.00f, 1.00f);
    s.Colors[ImGuiCol_CheckMark]    = ImVec4(0.40f, 0.80f, 0.40f, 1.00f);

    if (!ImGui_ImplGlfw_InitForOpenGL(window, true)) return false;
    if (!ImGui_ImplOpenGL3_Init("#version 410")) return false;
    m_initialized = true;
    return true;
}

void GUI::beginFrame() {
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    // Full-viewport dockspace
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::SetNextWindowViewport(vp->ID);
    ImGuiWindowFlags dsFlags =
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_MenuBar;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##DockSpace", nullptr, dsFlags);
    ImGui::PopStyleVar();
    ImGui::DockSpace(ImGui::GetID("MainDS"), ImVec2(0,0),
                     ImGuiDockNodeFlags_PassthruCentralNode);
    drawMenuBar();
    ImGui::End();
}

void GUI::render(BPMClock& clock, LayerManager& layers) {
    drawBPMPanel(clock);
    drawLayerPanel(layers);
}

void GUI::endFrame() {
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void GUI::shutdown() {
    if (!m_initialized) return;
    m_initialized = false;
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
}

// ── Menu bar ──────────────────────────────────────────────────────────────────

void GUI::drawMenuBar() {
    if (!ImGui::BeginMenuBar()) return;
    if (ImGui::BeginMenu("SpaceGen")) {
        if (ImGui::MenuItem("Quit", "Esc")) m_wantsQuit = true;
        ImGui::EndMenu();
    }
    ImGui::EndMenuBar();
}

// ── BPM panel ─────────────────────────────────────────────────────────────────

void GUI::drawBPMPanel(BPMClock& clock) {
    ImGui::SetNextWindowSize(ImVec2(280, kBPMPanelHeight), ImGuiCond_FirstUseEver);
    ImGui::Begin("BPM");

    // BPM slider
    float bpm = static_cast<float>(clock.bpm());
    if (ImGui::SliderFloat("BPM", &bpm, 20.0f, 300.0f, "%.1f")) {
        clock.setBPM(static_cast<double>(bpm));
    }

    // Beat phase indicator bar
    float phase = static_cast<float>(clock.phase());
    ImVec2 barMin = ImGui::GetCursorScreenPos();
    float  barW   = ImGui::GetContentRegionAvail().x;
    ImGui::GetWindowDrawList()->AddRectFilled(
        barMin,
        ImVec2(barMin.x + barW * phase, barMin.y + 8.0f),
        IM_COL32(80, 140, 255, 200)
    );
    ImGui::GetWindowDrawList()->AddRect(
        barMin,
        ImVec2(barMin.x + barW, barMin.y + 8.0f),
        IM_COL32(80, 80, 100, 255)
    );
    ImGui::Dummy(ImVec2(barW, 8.0f));

    // Tap tempo
    if (ImGui::Button("Tap", ImVec2(60, 0))) clock.tap();
    ImGui::SameLine();
    ImGui::Text("Phase: %.2f", phase);

    ImGui::End();
}

// ── Layer panel ───────────────────────────────────────────────────────────────

void GUI::drawLayerPanel(LayerManager& layers) {
    ImGui::SetNextWindowSize(ImVec2(320, 480), ImGuiCond_FirstUseEver);
    ImGui::Begin("Layers");

    int n = layers.layerCount();

    // Layer list
    ImGui::BeginChild("LayerList", ImVec2(0, 180), true);
    for (int i = 0; i < n; ++i) {
        ILayer* layer = layers.getLayer(i);
        if (!layer) continue;

        bool selected = (m_selectedLayer == i);
        bool enabled  = layer->enabled();

        ImGui::PushID(i);

        // Enabled checkbox
        if (ImGui::Checkbox("##en", &enabled)) layer->setEnabled(enabled);
        ImGui::SameLine();

        if (ImGui::Selectable(layer->name().c_str(), selected,
                              ImGuiSelectableFlags_AllowDoubleClick)) {
            m_selectedLayer = i;
        }

        // Drag-to-reorder
        if (ImGui::IsItemActive() && !ImGui::IsItemHovered()) {
            int delta = ImGui::GetMouseDragDelta(0).y < 0 ? -1 : 1;
            int dst   = i + delta;
            if (dst >= 0 && dst < n) {
                layers.moveLayer(i, dst);
                m_selectedLayer = dst;
                ImGui::ResetMouseDragDelta();
            }
        }

        ImGui::PopID();
    }
    ImGui::EndChild();

    // Per-layer controls
    if (m_selectedLayer >= 0 && m_selectedLayer < n) {
        ILayer* layer = layers.getLayer(m_selectedLayer);
        if (layer) {
            ImGui::Separator();

            float op = layer->opacity();
            if (ImGui::SliderFloat("Opacity", &op, 0.0f, 1.0f))
                layer->setOpacity(op);

            int bm = layer->blendMode();
            if (ImGui::Combo("Blend", &bm, kBlendNames, 4))
                layer->setBlendMode(bm);

            ImGui::Separator();
            // Delegate shader-specific controls to the layer itself
            if (ImGui::CollapsingHeader("Parameters", ImGuiTreeNodeFlags_DefaultOpen))
                layer->renderGUI();
        }
    }

    ImGui::End();
}
