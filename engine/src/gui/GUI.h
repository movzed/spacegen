#pragma once
#include "../core/BPMClock.h"
#include "../core/LayerManager.h"
#include <GLFW/glfw3.h>

class GUI {
public:
    GUI() = default;
    ~GUI() { shutdown(); }

    bool init(GLFWwindow* window);
    void beginFrame();
    void render(BPMClock& clock, LayerManager& layers);
    void endFrame();
    void shutdown();

    // Returns true when the user closes the app via GUI
    bool wantsQuit() const { return m_wantsQuit; }

private:
    void drawBPMPanel(BPMClock& clock);
    void drawLayerPanel(LayerManager& layers);
    void drawMenuBar();

    bool m_wantsQuit      = false;
    bool m_initialized    = false;
    int  m_selectedLayer  = 0;

    static constexpr float kBPMPanelHeight = 110.0f;
};
