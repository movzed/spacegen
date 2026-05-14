#include "LayerManager.h"
#include <cstdio>
#include <algorithm>

// Minimal inline shaders for the compositing blit pass
static const char* kCompVert = R"(
#version 410 core
out vec2 vUV;
void main() {
    vec2 pos = vec2((gl_VertexID & 1) * 4.0 - 1.0,
                    (gl_VertexID & 2) * 2.0 - 1.0);
    vUV = pos * 0.5 + 0.5;
    gl_Position = vec4(pos, 0.0, 1.0);
}
)";

static const char* kCompFrag = R"(
#version 410 core
in  vec2      vUV;
out vec4      fragColor;
uniform sampler2D uTex;
uniform float     uOpacity;
void main() {
    fragColor = texture(uTex, vUV) * uOpacity;
}
)";

static GLuint compileInline(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint len = 0; glGetShaderiv(s, GL_INFO_LOG_LENGTH, &len);
        std::string log(static_cast<size_t>(len), '\0');
        glGetShaderInfoLog(s, len, nullptr, log.data());
        fprintf(stderr, "LayerManager comp shader error:\n%s\n", log.c_str());
        glDeleteShader(s); return 0;
    }
    return s;
}

bool LayerManager::buildCompProgram() {
    GLuint vert = compileInline(GL_VERTEX_SHADER,   kCompVert);
    GLuint frag = compileInline(GL_FRAGMENT_SHADER, kCompFrag);
    if (!vert || !frag) return false;

    m_compProgram = glCreateProgram();
    glAttachShader(m_compProgram, vert);
    glAttachShader(m_compProgram, frag);
    glLinkProgram(m_compProgram);
    glDetachShader(m_compProgram, vert); glDeleteShader(vert);
    glDetachShader(m_compProgram, frag); glDeleteShader(frag);

    GLint ok = 0; glGetProgramiv(m_compProgram, GL_LINK_STATUS, &ok);
    if (!ok) { fprintf(stderr, "LayerManager: comp program link failed\n"); return false; }
    return true;
}

bool LayerManager::init(int width, int height) {
    m_width = width; m_height = height;
    GL_CHECK(glGenVertexArrays(1, &m_compVao));
    return buildCompProgram();
}

void LayerManager::update(float time, float delta, bool beat, double bpm) {
    for (auto& e : m_layers)
        if (e.layer->enabled())
            e.layer->update(time, delta, beat, bpm);
}

void LayerManager::render() {
    GL_CHECK(glEnable(GL_BLEND));

    for (auto& e : m_layers) {
        if (!e.layer->enabled()) continue;

        // Render layer into its own RT
        e.rt.bind();
        GL_CHECK(glClearColor(0.0f, 0.0f, 0.0f, 0.0f));
        GL_CHECK(glClear(GL_COLOR_BUFFER_BIT));
        e.layer->render();
        e.rt.unbind();

        // Composite onto default FB
        GL_CHECK(glBindFramebuffer(GL_FRAMEBUFFER, 0));
        GL_CHECK(glViewport(0, 0, m_width, m_height));

        applyBlend(e.layer->blendMode());

        GL_CHECK(glUseProgram(m_compProgram));
        GLint locTex = glGetUniformLocation(m_compProgram, "uTex");
        GLint locOp  = glGetUniformLocation(m_compProgram, "uOpacity");
        e.rt.bindTexture(0);
        if (locTex >= 0) GL_CHECK(glUniform1i(locTex, 0));
        if (locOp  >= 0) GL_CHECK(glUniform1f(locOp, e.layer->opacity()));

        GL_CHECK(glBindVertexArray(m_compVao));
        GL_CHECK(glDrawArrays(GL_TRIANGLES, 0, 3));
        GL_CHECK(glBindVertexArray(0));
        GL_CHECK(glUseProgram(0));
    }

    resetBlend();
}

void LayerManager::renderGUI() {
    for (int i = 0; i < (int)m_layers.size(); ++i)
        m_layers[i].layer->renderGUI();
}

void LayerManager::resize(int width, int height) {
    m_width = width; m_height = height;
    for (auto& e : m_layers) {
        e.rt.init(width, height);
        e.layer->resize(width, height);
    }
}

void LayerManager::shutdown() {
    for (auto& e : m_layers) e.layer->shutdown();
    m_layers.clear();
    if (m_compVao)     { GL_CHECK(glDeleteVertexArrays(1, &m_compVao)); m_compVao = 0; }
    if (m_compProgram) { GL_CHECK(glDeleteProgram(m_compProgram)); m_compProgram = 0; }
}

void LayerManager::addLayer(std::unique_ptr<ILayer> layer) {
    Entry e;
    e.rt.init(m_width, m_height);
    e.layer = std::move(layer);
    m_layers.push_back(std::move(e));
}

void LayerManager::removeLayer(int idx) {
    if (idx < 0 || idx >= (int)m_layers.size()) return;
    m_layers[idx].layer->shutdown();
    m_layers.erase(m_layers.begin() + idx);
}

void LayerManager::moveLayer(int from, int to) {
    if (from == to) return;
    int n = (int)m_layers.size();
    if (from < 0 || from >= n || to < 0 || to >= n) return;
    if (from < to)
        std::rotate(m_layers.begin() + from, m_layers.begin() + from + 1, m_layers.begin() + to + 1);
    else
        std::rotate(m_layers.begin() + to,   m_layers.begin() + from,     m_layers.begin() + from + 1);
}

ILayer* LayerManager::getLayer(int idx) {
    if (idx < 0 || idx >= (int)m_layers.size()) return nullptr;
    return m_layers[idx].layer.get();
}

// ── blend modes ───────────────────────────────────────────────────────────────

void LayerManager::applyBlend(int mode) {
    switch (mode) {
    case ILayer::BLEND_ADDITIVE:
        GL_CHECK(glBlendFunc(GL_ONE, GL_ONE));
        break;
    case ILayer::BLEND_NORMAL:
        GL_CHECK(glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
        break;
    case ILayer::BLEND_SCREEN:
        GL_CHECK(glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_COLOR));
        break;
    case ILayer::BLEND_MULTIPLY:
        GL_CHECK(glBlendFunc(GL_DST_COLOR, GL_ZERO));
        break;
    default:
        GL_CHECK(glBlendFunc(GL_ONE, GL_ONE));
        break;
    }
}

void LayerManager::resetBlend() {
    GL_CHECK(glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
    GL_CHECK(glDisable(GL_BLEND));
}
