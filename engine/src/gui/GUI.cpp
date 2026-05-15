#include "GUI.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"
#include <cmath>
#include <cstdio>

// ── palette ───────────────────────────────────────────────────────────────────
namespace Col {
    [[maybe_unused]] static const ImU32 BG = IM_COL32( 10,  10,  14, 255);
    static const ImU32 PanelBG     = IM_COL32( 18,  18,  26, 255);
    static const ImU32 SectionBG   = IM_COL32( 22,  22,  32, 255);
    static const ImU32 Border      = IM_COL32( 48,  48,  70, 255);
    static const ImU32 BorderBrt   = IM_COL32( 72,  72, 110, 255);
    static const ImU32 KnobBody    = IM_COL32( 42,  42,  58, 255);
    static const ImU32 KnobRing    = IM_COL32( 70,  70, 100, 255);
    static const ImU32 ArcBG       = IM_COL32( 50,  50,  68, 255);
    static const ImU32 ArcFill     = IM_COL32( 70, 130, 255, 220);
    [[maybe_unused]] static const ImU32 ArcFillAmb = IM_COL32(255, 160, 40, 220);
    static const ImU32 Indicator   = IM_COL32(220, 228, 255, 255);
    static const ImU32 LedGreen    = IM_COL32( 40, 220,  80, 255);
    [[maybe_unused]] static const ImU32 LedRed = IM_COL32(220, 50, 50, 255);
    static const ImU32 LedOff      = IM_COL32( 30,  36,  44, 255);
    static const ImU32 FaderSlot   = IM_COL32( 12,  12,  18, 255);
    static const ImU32 FaderHandle = IM_COL32(180, 190, 215, 255);
    static const ImU32 SelStrip    = IM_COL32( 30,  38,  70, 255);
    static const ImU32 StripBorder = IM_COL32( 38,  50,  90, 255);
    static const ImU32 TextDim     = IM_COL32(130, 130, 160, 255);
    static const ImU32 TextBrt     = IM_COL32(220, 224, 255, 255);
    static const ImU32 BPMAmber    = IM_COL32(255, 175,  30, 255);
    static const ImU32 BPMAmberDim = IM_COL32( 80,  55,   8, 255);
}

static const char* kBlendNames[] = { "ADD", "NRM", "SCR", "MUL" };
static const char* kWaveNames[]  = { "SIN", "TRI", "SAW", "SQR", "RND" };

// ─────────────────────────────────────────────────────────────────────────────
// Widget helpers
// ─────────────────────────────────────────────────────────────────────────────

// Drag-to-rotate knob. Returns true if changed.
// Knob arc: 7 o'clock (min) → clockwise 300° → 5 o'clock (max).
static bool Knob(const char* id, float* v, float vmin, float vmax,
                 const char* label, float size = 46.0f, ImU32 arcCol = Col::ArcFill)
{
    ImDrawList* dl  = ImGui::GetWindowDrawList();
    ImVec2      pos = ImGui::GetCursorScreenPos();
    ImVec2      ctr = {pos.x + size * 0.5f, pos.y + size * 0.5f};
    float       r   = size * 0.44f;
    float       t   = (vmax > vmin) ? ImSaturate((*v - vmin) / (vmax - vmin)) : 0.0f;

    // 7 o'clock in screen-space radians (ImGui: 0=right, clockwise positive)
    const float kA0 = 2.0f * IM_PI / 3.0f;          // 120° = 7 o'clock
    const float kA1 = kA0 + 5.0f * IM_PI / 3.0f;    // +300° = 5 o'clock
    float       aC  = kA0 + t * (kA1 - kA0);

    // Track
    dl->PathArcTo(ctr, r, kA0, kA1, 36);
    dl->PathStroke(Col::ArcBG, false, 2.5f);

    // Value fill
    if (t > 0.005f) {
        dl->PathArcTo(ctr, r, kA0, aC, 28);
        dl->PathStroke(arcCol, false, 3.0f);
    }

    // Body
    dl->AddCircleFilled(ctr, r * 0.58f, Col::KnobBody);
    dl->AddCircle      (ctr, r * 0.58f, Col::KnobRing, 24, 1.0f);

    // Indicator line
    float ix = ctr.x + cosf(aC) * r * 0.42f;
    float iy = ctr.y + sinf(aC) * r * 0.42f;
    dl->AddLine(ctr, {ix, iy}, Col::Indicator, 1.5f);

    // Hit area
    ImGui::SetCursorScreenPos(pos);
    ImGui::InvisibleButton(id, {size, size});
    bool changed = false;
    if (ImGui::IsItemActive()) {
        float d = -ImGui::GetIO().MouseDelta.y * 0.007f * (vmax - vmin);
        *v      = ImClamp(*v + d, vmin, vmax);
        changed = true;
    }
    if (ImGui::IsItemHovered()) {
        float w = ImGui::GetIO().MouseWheel;
        if (w != 0.0f) {
            *v    = ImClamp(*v + w * (vmax - vmin) * 0.02f, vmin, vmax);
            changed = true;
        }
        ImGui::SetTooltip("%s: %.3f", label, *v);
    }

    // Label centred below knob
    float labelY = pos.y + size + 1.0f;
    float tw     = ImGui::CalcTextSize(label).x;
    float lx     = pos.x + (size - tw) * 0.5f;
    ImGui::GetWindowDrawList()->AddText({lx, labelY}, Col::TextDim, label);
    ImGui::SetCursorScreenPos({pos.x + size + 6.0f, pos.y}); // advance right
    return changed;
}

// Small LED dot
static void LED(ImVec2 pos, float r, ImU32 col) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddCircleFilled(pos, r, col);
    dl->AddCircle      (pos, r, IM_COL32(0,0,0,120), 12, 0.8f);
}

// Section header bar (full-width coloured stripe + title)
static void SectionHeader(ImDrawList* dl, ImVec2 pos, float w, const char* title,
                           ImU32 accent = IM_COL32(70, 130, 255, 180))
{
    const float h = 16.0f;
    dl->AddRectFilled(pos, {pos.x + w, pos.y + h}, accent);
    float tw = ImGui::CalcTextSize(title).x;
    dl->AddText({pos.x + (w - tw) * 0.5f, pos.y + 1.0f},
                IM_COL32(220, 224, 255, 255), title);
}

// Amber 7-segment-style BPM number
static void BPMDisplay(ImDrawList* dl, ImVec2 pos, float w, float h, double bpm) {
    // Background LCD rectangle
    dl->AddRectFilled(pos, {pos.x + w, pos.y + h}, IM_COL32(8, 8, 6, 255));
    dl->AddRect      (pos, {pos.x + w, pos.y + h}, Col::Border);

    // Draw "ghost" segments dimly
    char ghost[] = "888";
    float ghostW = ImGui::CalcTextSize(ghost).x * 2.2f;
    float ghostX = pos.x + (w - ghostW) * 0.5f;
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 2.2f,
                {ghostX, pos.y + (h - ImGui::GetFontSize() * 2.2f) * 0.5f},
                Col::BPMAmberDim, ghost);

    // Real value
    char buf[16];
    snprintf(buf, sizeof(buf), "%3.0f", bpm);
    float valW = ImGui::CalcTextSize(buf).x * 2.2f;
    float valX = pos.x + (w - valW) * 0.5f;
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 2.2f,
                {valX, pos.y + (h - ImGui::GetFontSize() * 2.2f) * 0.5f},
                Col::BPMAmber, buf);
}

// ─────────────────────────────────────────────────────────────────────────────
// GUI lifecycle
// ─────────────────────────────────────────────────────────────────────────────

bool GUI::init(GLFWwindow* window) {
    if (m_initialized) return true;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags    |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename     = "spacegen_layout.ini";
    io.ConfigWindowsMoveFromTitleBarOnly = true;

    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 0.0f;
    s.FrameRounding  = 2.0f;
    s.GrabRounding   = 2.0f;
    s.ItemSpacing    = {4.0f, 3.0f};
    s.WindowPadding  = {0.0f, 0.0f};
    s.Colors[ImGuiCol_WindowBg]       = ImVec4(0.07f, 0.07f, 0.10f, 1.0f);
    s.Colors[ImGuiCol_FrameBg]        = ImVec4(0.10f, 0.10f, 0.15f, 1.0f);
    s.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.15f, 0.15f, 0.22f, 1.0f);
    s.Colors[ImGuiCol_SliderGrab]     = ImVec4(0.70f, 0.75f, 0.84f, 1.0f);
    s.Colors[ImGuiCol_SliderGrabActive]=ImVec4(0.85f, 0.90f, 1.00f, 1.0f);
    s.Colors[ImGuiCol_Button]         = ImVec4(0.16f, 0.16f, 0.24f, 1.0f);
    s.Colors[ImGuiCol_ButtonHovered]  = ImVec4(0.24f, 0.30f, 0.48f, 1.0f);
    s.Colors[ImGuiCol_ButtonActive]   = ImVec4(0.30f, 0.40f, 0.70f, 1.0f);
    s.Colors[ImGuiCol_Header]         = ImVec4(0.20f, 0.25f, 0.40f, 1.0f);
    s.Colors[ImGuiCol_PopupBg]        = ImVec4(0.10f, 0.10f, 0.16f, 0.98f);
    s.Colors[ImGuiCol_Border]         = ImVec4(0.20f, 0.20f, 0.30f, 1.0f);

    if (!ImGui_ImplGlfw_InitForOpenGL(window, true)) return false;
    if (!ImGui_ImplOpenGL3_Init("#version 410"))      return false;
    m_initialized = true;
    return true;
}

void GUI::beginFrame() {
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

void GUI::render(BPMClock& clock, LayerManager& layers) {
    ImGuiIO& io = ImGui::GetIO();
    m_vpW = io.DisplaySize.x;
    m_vpH = io.DisplaySize.y;

    const float topH = m_vpH * kTopH;
    const float midH = m_vpH * kMidH;
    const float botH = m_vpH - topH - midH;

    drawTopZone   (0, 0,    m_vpW, topH, clock);
    drawMiddleZone(0, topH, m_vpW, midH, layers);
    drawBottomZone(0, topH + midH, m_vpW, botH, layers);
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

// ─────────────────────────────────────────────────────────────────────────────
// Zone helpers — each zone is a single borderless ImGui window
// ─────────────────────────────────────────────────────────────────────────────

static void BeginZone(const char* id, float x, float y, float w, float h, ImU32 bg) {
    ImGui::SetNextWindowPos ({x, y});
    ImGui::SetNextWindowSize({w, h});
    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNav;
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(bg));
    ImGui::Begin(id, nullptr, kFlags);
}

static void EndZone() {
    ImGui::End();
    ImGui::PopStyleColor();
}

// ─────────────────────────────────────────────────────────────────────────────
// TOP ZONE — Master FX + BPM / Transport
// ─────────────────────────────────────────────────────────────────────────────

void GUI::drawTopZone(float x, float y, float w, float h, BPMClock& clock) {
    BeginZone("##TopZone", x, y, w, h, Col::PanelBG);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2      wp = ImGui::GetWindowPos();

    // Horizontal split: master FX takes ~70%, BPM takes ~30%
    const float fxW  = w * 0.70f;
    const float bpmW = w - fxW;

    // Section borders
    dl->AddRectFilled({wp.x, wp.y}, {wp.x + w, wp.y + h}, Col::PanelBG);
    dl->AddLine({wp.x + fxW, wp.y + 18}, {wp.x + fxW, wp.y + h - 4}, Col::Border, 1.0f);
    dl->AddLine({wp.x, wp.y + h - 1}, {wp.x + w, wp.y + h - 1}, Col::BorderBrt, 1.0f);

    drawMasterFX(wp.x, wp.y, fxW, h);
    drawBPMPanel(wp.x + fxW, wp.y, bpmW, h, clock);

    EndZone();
}

void GUI::drawMasterFX(float x, float y, float w, float h) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    SectionHeader(dl, {x, y}, w, "MASTER  EFFECTS",
                  IM_COL32(55, 90, 180, 200));

    const float knobSize = 46.0f;
    const float labelH   = 14.0f;
    const float totalW   = 8.0f * (knobSize + 6.0f);
    const float startX   = x + (w - totalW) * 0.5f;
    const float knoby    = y + 16.0f + (h - 16.0f - knobSize - labelH) * 0.5f;

    ImGui::SetCursorScreenPos({startX, knoby});

    struct KnobDef { const char* id; const char* label; float* val; float lo; float hi; };
    KnobDef defs[] = {
        {"k_bri", "BRIGHT",  &m_masterFX.brightness, -1.0f, 1.0f},
        {"k_con", "CONTR",   &m_masterFX.contrast,    0.0f, 3.0f},
        {"k_sat", "SAT",     &m_masterFX.saturation,  0.0f, 3.0f},
        {"k_hue", "HUE",     &m_masterFX.hue,         0.0f, 1.0f},
        {"k_glo", "GLOW",    &m_masterFX.glow,        0.0f, 1.0f},
        {"k_vig", "VIGNETTE",&m_masterFX.vignette,    0.0f, 1.0f},
        {"k_fb",  "FEEDBK",  &m_masterFX.feedback,    0.0f, 1.0f},
        {"k_ab",  "ABERR",   &m_masterFX.aberration,  0.0f, 1.0f},
    };
    for (auto& d : defs)
        Knob(d.id, d.val, d.lo, d.hi, d.label, knobSize, Col::ArcFill);
}

void GUI::drawBPMPanel(float x, float y, float w, float h, BPMClock& clock) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    SectionHeader(dl, {x, y}, w, "BPM / TRANSPORT",
                  IM_COL32(150, 90, 20, 200));

    float bpm = static_cast<float>(clock.bpm());

    // LCD display
    const float dispH = h * 0.38f;
    const float dispW = w * 0.70f;
    const float dispX = x + (w - dispW) * 0.5f;
    const float dispY = y + 18.0f + 4.0f;
    BPMDisplay(dl, {dispX, dispY}, dispW, dispH, bpm);

    // Beat phase bar below LCD
    float phase = static_cast<float>(clock.phase());
    const float barY = dispY + dispH + 4.0f;
    const float barH = 5.0f;
    const float barX = x + 8.0f;
    const float barW = w - 16.0f;
    dl->AddRectFilled({barX, barY}, {barX + barW, barY + barH},
                      IM_COL32(20, 20, 30, 255));
    dl->AddRectFilled({barX, barY}, {barX + barW * phase, barY + barH},
                      Col::BPMAmber);
    dl->AddRect({barX, barY}, {barX + barW, barY + barH}, Col::Border);

    // BPM slider
    const float sliderY = barY + barH + 6.0f;
    ImGui::SetCursorScreenPos({barX, sliderY});
    ImGui::PushItemWidth(barW);
    ImGui::PushStyleColor(ImGuiCol_SliderGrab,       ImGui::ColorConvertU32ToFloat4(Col::BPMAmber));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImGui::ColorConvertU32ToFloat4(Col::BPMAmber));
    if (ImGui::SliderFloat("##bpm", &bpm, 20.0f, 300.0f, "%.1f BPM"))
        clock.setBPM(static_cast<double>(bpm));
    ImGui::PopStyleColor(2);
    ImGui::PopItemWidth();

    // Tap button
    const float tapY = sliderY + 22.0f;
    const float tapW = w - 16.0f;
    ImGui::SetCursorScreenPos({barX, tapY});
    ImGui::PushStyleColor(ImGuiCol_Button,        ImGui::ColorConvertU32ToFloat4(IM_COL32(60, 40, 10, 255)));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::ColorConvertU32ToFloat4(IM_COL32(100, 70, 20, 255)));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImGui::ColorConvertU32ToFloat4(Col::BPMAmber));
    if (ImGui::Button("TAP TEMPO", {tapW, 22.0f})) clock.tap();
    ImGui::PopStyleColor(3);

    // Beat LED
    float ledX = x + w - 14.0f;
    float ledY = y + 18.0f + dispH * 0.5f + 6.0f;
    bool  beat  = phase < 0.08f;
    LED({ledX, ledY}, 5.0f, beat ? Col::BPMAmber : IM_COL32(50, 32, 5, 255));
}

// ─────────────────────────────────────────────────────────────────────────────
// MIDDLE ZONE — LFO + Layer Params
// ─────────────────────────────────────────────────────────────────────────────

void GUI::drawMiddleZone(float x, float y, float w, float h, LayerManager& layers) {
    BeginZone("##MidZone", x, y, w, h, Col::SectionBG);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2      wp = ImGui::GetWindowPos();

    const float lfoW    = w * 0.28f;
    const float paramsW = w - lfoW;

    dl->AddLine({wp.x, wp.y + h - 1}, {wp.x + w, wp.y + h - 1}, Col::BorderBrt);
    dl->AddLine({wp.x + lfoW, wp.y + 18}, {wp.x + lfoW, wp.y + h - 4}, Col::Border);

    drawLFOPanel  (wp.x,        wp.y, lfoW,   h);
    drawLayerParams(wp.x + lfoW, wp.y, paramsW, h, layers);

    EndZone();
}

void GUI::drawLFOPanel(float x, float y, float w, float /*h*/) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    SectionHeader(dl, {x, y}, w, "LFO",
                  IM_COL32(80, 160, 80, 200));

    const float knobSize = 40.0f;
    const float totalW   = 3.0f * (knobSize + 6.0f);
    const float startX   = x + (w - totalW) * 0.5f;
    const float knoby    = y + 17.0f + 8.0f;

    // Wave selector (small combo-style buttons)
    float btnW = (w - 16.0f) / 5.0f;
    float btnY = knoby;
    ImGui::SetCursorScreenPos({x + 8.0f, btnY});
    for (int i = 0; i < 5; ++i) {
        ImGui::PushID(i + 200);
        bool sel = (m_lfoWave == i);
        if (sel) ImGui::PushStyleColor(ImGuiCol_Button,
                     ImGui::ColorConvertU32ToFloat4(IM_COL32(50, 110, 50, 255)));
        if (ImGui::Button(kWaveNames[i], {btnW - 2.0f, 20.0f})) m_lfoWave = i;
        if (sel) ImGui::PopStyleColor();
        ImGui::SameLine(0, 2.0f);
        ImGui::PopID();
    }

    // Speed + Depth knobs
    ImGui::SetCursorScreenPos({startX, knoby + 28.0f});
    Knob("lfoSpd", &m_lfoSpeed, 0.05f, 16.0f, "SPEED", knobSize,
         IM_COL32(60, 190, 80, 220));
    Knob("lfoDep", &m_lfoDepth, 0.0f,  1.0f,  "DEPTH", knobSize,
         IM_COL32(60, 190, 80, 220));
}

void GUI::drawLayerParams(float x, float y, float w, float h, LayerManager& layers) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    SectionHeader(dl, {x, y}, w,
                  (m_selectedLayer < layers.layerCount())
                     ? layers.getLayer(m_selectedLayer)->name().c_str()
                     : "LAYER PARAMS",
                  IM_COL32(70, 110, 200, 200));

    if (m_selectedLayer >= layers.layerCount()) {
        EndZone(); // shouldn't happen, but safe
        return;
    }
    ILayer* layer = layers.getLayer(m_selectedLayer);
    if (!layer) return;

    // Render the layer's custom GUI inside a child region
    ImGui::SetCursorScreenPos({x + 8.0f, y + 20.0f});
    ImGui::BeginChild("##LayerParamsChild",
                      {w - 16.0f, h - 24.0f},
                      false,
                      ImGuiWindowFlags_NoScrollbar);
    layer->renderGUI();
    ImGui::EndChild();
}

// ─────────────────────────────────────────────────────────────────────────────
// BOTTOM ZONE — Channel strips with vertical faders
// ─────────────────────────────────────────────────────────────────────────────

void GUI::drawBottomZone(float x, float y, float w, float h, LayerManager& layers) {
    BeginZone("##BotZone", x, y, w, h, Col::PanelBG);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2      wp = ImGui::GetWindowPos();

    SectionHeader(dl, {wp.x, wp.y}, w, "LAYERS",
                  IM_COL32(55, 55, 130, 200));
    dl->AddLine({wp.x, wp.y + h - 1}, {wp.x + w, wp.y + h - 1}, Col::Border);

    // Determine how many strips to show (min 8 visible slots)
    const int kSlots = 8;
    const float stripW = w / static_cast<float>(kSlots);

    for (int i = 0; i < kSlots; ++i) {
        float sx = wp.x + i * stripW;
        float sy = wp.y;
        drawChannelStrip(sx, sy, stripW, h, i, layers);
    }

    EndZone();
}

void GUI::drawChannelStrip(float x, float y, float w, float h,
                           int idx, LayerManager& layers)
{
    ImDrawList* dl   = ImGui::GetWindowDrawList();
    bool        hasLayer = (idx < layers.layerCount());
    ILayer*     layer    = hasLayer ? layers.getLayer(idx) : nullptr;
    bool        selected = (idx == m_selectedLayer && hasLayer);

    // Strip background
    ImU32 stripBG = selected ? Col::SelStrip : Col::PanelBG;
    dl->AddRectFilled({x, y + 17.0f}, {x + w, y + h}, stripBG);
    dl->AddLine({x + w - 1, y + 17.0f}, {x + w - 1, y + h}, Col::Border);

    if (selected)
        dl->AddRect({x, y + 17.0f}, {x + w - 1, y + h - 1}, Col::StripBorder);

    if (!hasLayer) {
        // Empty slot
        dl->AddText({x + 8.0f, y + h * 0.5f}, Col::TextDim, "---");
        return;
    }

    // Click to select
    ImGui::SetCursorScreenPos({x, y + 17.0f});
    ImGui::InvisibleButton(("##sel" + std::to_string(idx)).c_str(),
                           {w, h - 17.0f});
    if (ImGui::IsItemClicked()) m_selectedLayer = idx;

    const float pad    = 5.0f;
    const float inner  = w - pad * 2.0f;
    float       cy     = y + 20.0f;   // current y cursor

    // ── Enable LED ─────────────────────────────────────────────────────────
    bool enabled = layer->enabled();
    ImVec2 ledPos = {x + w * 0.5f, cy + 5.0f};
    LED(ledPos, 5.0f, enabled ? Col::LedGreen : Col::LedOff);

    // Invisible button over LED for toggle
    ImGui::SetCursorScreenPos({ledPos.x - 8.0f, ledPos.y - 8.0f});
    if (ImGui::InvisibleButton(("##led" + std::to_string(idx)).c_str(), {16.0f, 16.0f}))
        layer->setEnabled(!enabled);
    cy += 16.0f;

    // ── Layer name ─────────────────────────────────────────────────────────
    const std::string& name = layer->name();
    std::string shortName   = name.length() > 8 ? name.substr(0, 7) + "…" : name;
    float nw = ImGui::CalcTextSize(shortName.c_str()).x;
    dl->AddText({x + (w - nw) * 0.5f, cy}, selected ? Col::TextBrt : Col::TextDim,
                shortName.c_str());
    cy += 14.0f;

    // ── Blend mode (tiny cycling button) ───────────────────────────────────
    int bm = layer->blendMode();
    ImGui::SetCursorScreenPos({x + pad, cy});
    ImGui::PushID(idx * 100 + 1);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {2.0f, 1.0f});
    if (ImGui::Button(kBlendNames[bm], {inner, 16.0f}))
        layer->setBlendMode((bm + 1) % 4);
    ImGui::PopStyleVar();
    ImGui::PopID();
    cy += 18.0f;

    // ── Vertical fader ─────────────────────────────────────────────────────
    // Reserve bottom area for fader + value label
    const float valLabelH = 16.0f;
    const float faderH    = h - (cy - y) - valLabelH - pad * 2.0f;
    const float faderW    = 18.0f;
    const float faderX    = x + (w - faderW) * 0.5f;
    const float faderY    = cy + 4.0f;

    // Slot background
    dl->AddRectFilled({faderX, faderY},
                      {faderX + faderW, faderY + faderH},
                      Col::FaderSlot, 2.0f);
    dl->AddRect({faderX, faderY},
                {faderX + faderW, faderY + faderH},
                Col::Border, 2.0f);

    float op = layer->opacity();
    ImGui::SetCursorScreenPos({faderX, faderY});
    ImGui::PushID(idx * 100 + 2);
    ImGui::PushStyleColor(ImGuiCol_FrameBg,        ImGui::ColorConvertU32ToFloat4(Col::FaderSlot));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab,     ImGui::ColorConvertU32ToFloat4(Col::FaderHandle));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImGui::ColorConvertU32ToFloat4(IM_COL32(255,255,255,255)));
    if (ImGui::VSliderFloat("##fader", {faderW, faderH}, &op, 0.0f, 1.0f, ""))
        layer->setOpacity(op);
    ImGui::PopStyleColor(3);
    ImGui::PopID();

    // Opacity percentage label
    char opBuf[8];
    snprintf(opBuf, sizeof(opBuf), "%3d%%", static_cast<int>(op * 100.0f));
    float ow = ImGui::CalcTextSize(opBuf).x;
    dl->AddText({x + (w - ow) * 0.5f, faderY + faderH + 3.0f},
                Col::TextDim, opBuf);
}
