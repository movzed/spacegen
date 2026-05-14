#include "ShaderLayer.h"
#include <cstdio>
#include <algorithm>
#include <filesystem>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

// ── public ────────────────────────────────────────────────────────────────────

void ShaderLayer::setShaderPaths(const std::string& vert, const std::string& frag) {
    m_shader.load(vert, frag);
}

bool ShaderLayer::init(int width, int height) {
    m_width  = width;
    m_height = height;
    // Null VAO — fullscreen triangle is generated in the vertex shader
    GL_CHECK(glGenVertexArrays(1, &m_vao));

    // 1×1 opaque white fallback texture for uMask when no mask is loaded
    GL_CHECK(glGenTextures(1, &m_whiteTex));
    GL_CHECK(glBindTexture(GL_TEXTURE_2D, m_whiteTex));
    const unsigned char white[4] = {255, 255, 255, 255};
    GL_CHECK(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white));
    GL_CHECK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST));
    GL_CHECK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST));
    GL_CHECK(glBindTexture(GL_TEXTURE_2D, 0));
    return true;
}

void ShaderLayer::update(float time, float delta, bool beat, double bpm) {
    m_shader.checkReload();
    advanceMask(delta, bpm);
    m_time = time; m_delta = delta; m_beat = beat; m_bpm = bpm;
}

void ShaderLayer::render() {
    m_shader.bind();

    // Common uniforms every shader expects
    m_shader.set("uResolution", glm::vec2(static_cast<float>(m_width),
                                          static_cast<float>(m_height)));
    m_shader.set("uOpacity", m_opacity);

    // Shader-specific uniforms — called here so the program is already bound
    onSetUniforms(m_time, m_delta, m_beat, m_bpm);

    uploadMask();

    GL_CHECK(glBindVertexArray(m_vao));
    GL_CHECK(glDrawArrays(GL_TRIANGLES, 0, 3));
    GL_CHECK(glBindVertexArray(0));

    m_shader.unbind();
}

void ShaderLayer::renderGUI() {
    onRenderGUI();
}

void ShaderLayer::resize(int width, int height) {
    m_width  = width;
    m_height = height;
}

void ShaderLayer::shutdown() {
    if (m_vao)      { GL_CHECK(glDeleteVertexArrays(1, &m_vao));    m_vao = 0; }
    if (m_whiteTex) { GL_CHECK(glDeleteTextures(1, &m_whiteTex)); m_whiteTex = 0; }
    for (GLuint t : m_mask.textures) GL_CHECK(glDeleteTextures(1, &t));
    m_mask.textures.clear();
    m_mask.loaded = false;
}

// ── mask loading ──────────────────────────────────────────────────────────────

void ShaderLayer::loadMask(const std::string& directory) {
    // Collect PNGs sorted by filename
    std::vector<std::filesystem::path> files;
    for (auto& e : std::filesystem::directory_iterator(directory)) {
        if (e.path().extension() == ".png")
            files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());

    if (files.empty()) {
        fprintf(stderr, "ShaderLayer: no PNGs in '%s'\n", directory.c_str());
        return;
    }

    // Free previous
    for (GLuint t : m_mask.textures) GL_CHECK(glDeleteTextures(1, &t));
    m_mask.textures.clear();

    stbi_set_flip_vertically_on_load(true);
    for (auto& path : files) {
        int w, h, ch;
        unsigned char* data = stbi_load(path.c_str(), &w, &h, &ch, 4);
        if (!data) { fprintf(stderr, "stbi failed: %s\n", path.c_str()); continue; }

        GLuint tex;
        GL_CHECK(glGenTextures(1, &tex));
        GL_CHECK(glBindTexture(GL_TEXTURE_2D, tex));
        GL_CHECK(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data));
        GL_CHECK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
        GL_CHECK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
        GL_CHECK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
        GL_CHECK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
        stbi_image_free(data);

        m_mask.textures.push_back(tex);
    }

    m_mask.frame  = 0;
    m_mask.accum  = 0.0f;
    m_mask.loaded = !m_mask.textures.empty();
    fprintf(stdout, "Mask loaded: %zu frames from '%s'\n", m_mask.textures.size(), directory.c_str());
}

// ── private ───────────────────────────────────────────────────────────────────

void ShaderLayer::advanceMask(float delta, double bpm) {
    if (!m_mask.loaded || m_mask.textures.empty()) return;
    if (bpm <= 0.0) return;

    int n = static_cast<int>(m_mask.textures.size());
    float framesPerBeat = static_cast<float>(n) / m_mask.beatsPerLoop;
    float fps = static_cast<float>(bpm) / 60.0f * framesPerBeat;

    m_mask.accum += delta * fps;
    while (m_mask.accum >= 1.0f) {
        m_mask.accum -= 1.0f;
        m_mask.frame  = (m_mask.frame + 1) % n;
    }
}

void ShaderLayer::uploadMask() {
    GL_CHECK(glActiveTexture(GL_TEXTURE1));
    if (m_mask.loaded && !m_mask.textures.empty()) {
        GL_CHECK(glBindTexture(GL_TEXTURE_2D, m_mask.textures[m_mask.frame]));
        m_shader.set("uHasMask", true);
    } else {
        GL_CHECK(glBindTexture(GL_TEXTURE_2D, m_whiteTex));
        m_shader.set("uHasMask", false);
    }
    m_shader.set("uMask", 1);
}
