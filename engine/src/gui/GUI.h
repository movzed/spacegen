#pragma once
#include "../core/BPMClock.h"
#include "../core/LayerManager.h"
#include "../core/MasterFX.h"
#include <GLFW/glfw3.h>

// ─────────────────────────────────────────────────────────────────────────────
// GUI — RS7000-inspired fixed-layout instrument panel.
//
// Three fixed horizontal zones (top → bottom):
//   Top    — Master FX knobs + BPM / transport
//   Middle — Selected-layer params + LFO controls
//   Bottom — Layer channel strips with vertical opacity faders
// ─────────────────────────────────────────────────────────────────────────────
class GUI {
public:
    GUI()  = default;
    ~GUI() { shutdown(); }

    bool init(GLFWwindow* window);
    void beginFrame();
    void render(BPMClock& clock, LayerManager& layers);
    void endFrame();
    void shutdown();

    bool            wantsQuit() const { return m_wantsQuit; }
    const MasterFX& masterFX()  const { return m_masterFX; }

private:
    // ── layout helpers ────────────────────────────────────────────────────────
    void drawTopZone   (float x, float y, float w, float h, BPMClock& clock);
    void drawMiddleZone(float x, float y, float w, float h, LayerManager& layers);
    void drawBottomZone(float x, float y, float w, float h, LayerManager& layers);

    void drawMasterFX  (float x, float y, float w, float h);
    void drawBPMPanel  (float x, float y, float w, float h, BPMClock& clock);
    void drawLFOPanel  (float x, float y, float w, float h);
    void drawLayerParams(float x, float y, float w, float h, LayerManager& layers);
    void drawChannelStrip(float x, float y, float w, float h, int idx,
                          LayerManager& layers);

    // ── state ─────────────────────────────────────────────────────────────────
    MasterFX m_masterFX;

    // Per-strip LFO (stub – wired in Phase 2)
    float m_lfoSpeed = 1.0f;
    float m_lfoDepth = 0.0f;
    int   m_lfoWave  = 0;   // 0=sin 1=tri 2=saw 3=sqr 4=rnd

    int   m_selectedLayer = 0;
    bool  m_wantsQuit     = false;
    bool  m_initialized   = false;

    // proportions (computed in render)
    float m_vpW = 1280, m_vpH = 720;
    static constexpr float kTopH    = 0.27f;  // 27% of viewport height
    static constexpr float kMidH    = 0.23f;  // 23%
    // bottom = remaining 50%
};
