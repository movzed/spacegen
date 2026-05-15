#pragma once
#include "../layers/ILayer.h"
#include "MasterFX.h"
#include "RenderTarget.h"
#include "gl.h"
#include <vector>
#include <memory>
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// LayerManager — owns the ordered layer stack.
//
// Each layer renders into its own RenderTarget; LayerManager then composites
// them onto the default framebuffer using the layer's blend mode + opacity.
// ─────────────────────────────────────────────────────────────────────────────
class LayerManager {
public:
    LayerManager() = default;
    ~LayerManager() { shutdown(); }

    bool init(int width, int height);
    void update(float time, float delta, bool beat, double bpm);
    void render(const MasterFX& fx = {});
    void renderGUI();
    void resize(int width, int height);
    void shutdown();

    // Layer management
    void addLayer(std::unique_ptr<ILayer> layer);
    void removeLayer(int index);
    void moveLayer(int from, int to);
    ILayer* getLayer(int index);
    int     layerCount() const { return static_cast<int>(m_layers.size()); }

private:
    struct Entry {
        std::unique_ptr<ILayer>  layer;
        RenderTarget             rt;
    };

    std::vector<Entry> m_layers;
    int m_width  = 0;
    int m_height = 0;

    // Compositing pass
    GLuint m_compVao     = 0;
    GLuint m_compProgram = 0;

    // Master FX post-proc
    GLuint m_fxVao       = 0;
    GLuint m_fxProgram   = 0;
    RenderTarget m_fxRT;              // composite target before post-proc

    bool buildCompProgram();
    bool buildFXProgram();
    void applyBlend(int mode);
    void resetBlend();
};
