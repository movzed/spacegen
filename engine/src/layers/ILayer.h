#pragma once
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// ILayer — pure interface every SpaceGen layer must implement.
//
// Lifecycle:
//   init()       → allocate GPU resources (VAO, textures, FBOs)
//   update()     → upload uniforms; react to BPM; advance mask frame
//   render()     → draw into the currently bound FBO
//   renderGUI()  → draw Dear ImGui controls inside an already-open CollapsingHeader
//   resize()     → reallocate resolution-dependent GPU resources
//   shutdown()   → free all GPU resources
// ─────────────────────────────────────────────────────────────────────────────
class ILayer {
public:
    virtual ~ILayer() = default;

    virtual bool init(int width, int height)                          = 0;
    virtual void update(float time, float delta, bool beat, double bpm) = 0;
    virtual void render()                                             = 0;
    virtual void renderGUI()                                          = 0;
    virtual void resize(int width, int height)                        = 0;
    virtual void shutdown()                                           = 0;

    // Common state — managed by LayerManager via setters.
    const std::string& name()    const { return m_name; }
    bool               enabled() const { return m_enabled; }
    float              opacity() const { return m_opacity; }
    int                blendMode()const{ return m_blendMode; }

    void setEnabled  (bool e)  { m_enabled   = e; }
    void setOpacity  (float o) { m_opacity   = o; }
    void setBlendMode(int   m) { m_blendMode = m; }

    // Blend mode indices (matches LayerManager::applyBlend)
    static constexpr int BLEND_ADDITIVE = 0;
    static constexpr int BLEND_NORMAL   = 1;
    static constexpr int BLEND_SCREEN   = 2;
    static constexpr int BLEND_MULTIPLY = 3;

protected:
    explicit ILayer(const std::string& name) : m_name(name) {}

    std::string m_name;
    bool        m_enabled   = true;
    float       m_opacity   = 1.0f;
    int         m_blendMode = BLEND_ADDITIVE;
};
