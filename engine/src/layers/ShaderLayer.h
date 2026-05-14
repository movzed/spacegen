#pragma once
#include "ILayer.h"
#include "../core/ShaderProgram.h"
#include "../core/gl.h"
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// ShaderLayer — base class for all fullscreen GLSL shader layers.
//
// Manages:
//   • A "null" VAO for the fullscreen triangle (3 verts, no VBO)
//   • ShaderProgram with hot-reload
//   • Mask texture slot (uMask, uHasMask)
//   • Opacity uniform (uOpacity)
//
// Subclasses override:
//   onSetUniforms(time, delta, beat)  → upload shader-specific uniforms
//   onRenderGUI()                     → ImGui controls
// ─────────────────────────────────────────────────────────────────────────────
class ShaderLayer : public ILayer {
public:
    explicit ShaderLayer(const std::string& name) : ILayer(name) {}
    ~ShaderLayer() override { shutdown(); }

    // Pass the paths relative to the working directory (build/ at runtime).
    void setShaderPaths(const std::string& vert, const std::string& frag);

    // ILayer
    bool init(int width, int height)                              override;
    void update(float time, float delta, bool beat, double bpm)   override;
    void render()                                                 override;
    void renderGUI()                                              override;
    void resize(int width, int height)                            override;
    void shutdown()                                               override;

    // Load a PNG directory as a mask sequence. Frames advance with BPM.
    void loadMask(const std::string& directory);

protected:
    // Subclasses implement these
    virtual void onSetUniforms(float time, float delta, bool beat, double bpm) = 0;
    virtual void onRenderGUI() = 0;

    ShaderProgram m_shader;
    int           m_width  = 0;
    int           m_height = 0;

private:
    GLuint m_vao        = 0;
    GLuint m_whiteTex   = 0;   // 1×1 fallback when no mask loaded

    // Last update state — consumed by render() so uniforms are set with shader bound
    float  m_time  = 0.0f;
    float  m_delta = 0.0f;
    bool   m_beat  = false;
    double m_bpm   = 120.0;

    // Mask sequence
    struct MaskSequence {
        std::vector<GLuint> textures;
        int    frame    = 0;
        float  accum    = 0.0f;
        float  beatsPerLoop = 1.0f;
        bool   loaded   = false;
    } m_mask;

    void advanceMask(float delta, double bpm);
    void uploadMask();
};
