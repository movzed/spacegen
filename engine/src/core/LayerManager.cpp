#include "LayerManager.h"
#include <cstdio>
#include <algorithm>

// ── Inline GLSL shaders ───────────────────────────────────────────────────────

static const char* kFullscreenVert = R"(
#version 410 core
out vec2 vUV;
void main() {
    vec2 pos = vec2((gl_VertexID == 1) ? 3.0 : -1.0,
                    (gl_VertexID == 2) ? 3.0 : -1.0);
    vUV = pos * 0.5 + 0.5;
    gl_Position = vec4(pos, 0.0, 1.0);
}
)";

// Layer compositing blit — applies per-layer opacity
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

// Master post-processing — brightness/contrast/saturation/hue/glow/vignette
static const char* kFXFrag = R"(
#version 410 core
in  vec2      vUV;
out vec4      fragColor;
uniform sampler2D uTex;
uniform float uBrightness;
uniform float uContrast;
uniform float uSaturation;
uniform float uHue;
uniform float uGlow;
uniform float uVignette;
uniform float uAberration;

// RGB <-> HSV
vec3 rgb2hsv(vec3 c) {
    vec4 K = vec4(0.0, -1.0/3.0, 2.0/3.0, -1.0);
    vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));
    vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));
    float d = q.x - min(q.w, q.y);
    float e = 1e-10;
    return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}
vec3 hsv2rgb(vec3 c) {
    vec4 K = vec4(1.0, 2.0/3.0, 1.0/3.0, 3.0);
    vec3 p = abs(fract(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * mix(K.xxx, clamp(p - K.xxx, 0.0, 1.0), c.y);
}

void main() {
    // Chromatic aberration
    float ab = uAberration * 0.012;
    vec2  d  = (vUV - 0.5) * ab;
    float r = texture(uTex, vUV - d).r;
    float g = texture(uTex, vUV    ).g;
    float b = texture(uTex, vUV + d).b;
    vec3 col = vec3(r, g, b);

    // Hue shift
    if (abs(uHue) > 0.001) {
        vec3 hsv = rgb2hsv(col);
        hsv.x    = fract(hsv.x + uHue);
        col      = hsv2rgb(hsv);
    }

    // Brightness + contrast
    col = (col - 0.5) * uContrast + 0.5 + uBrightness;

    // Saturation
    float lum = dot(col, vec3(0.299, 0.587, 0.114));
    col = mix(vec3(lum), col, uSaturation);

    // Glow (bright-pass boost)
    vec3 bright = max(col - 0.65, 0.0);
    col += bright * uGlow * 3.0;

    // Vignette
    vec2  uvc  = vUV - 0.5;
    float dist = length(uvc);
    float vign = 1.0 - smoothstep(0.25, 0.75, dist * (1.0 + uVignette * 1.5));
    col *= vign;

    fragColor = vec4(clamp(col, 0.0, 4.0), 1.0);
}
)";

// ── compile helper ────────────────────────────────────────────────────────────

static GLuint compileInline(GLenum type, const char* src, const char* tag) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint len = 0; glGetShaderiv(s, GL_INFO_LOG_LENGTH, &len);
        std::string log(static_cast<size_t>(len), '\0');
        glGetShaderInfoLog(s, len, nullptr, log.data());
        fprintf(stderr, "LayerManager [%s] compile error:\n%s\n", tag, log.c_str());
        glDeleteShader(s); return 0;
    }
    return s;
}

static GLuint linkProgram(GLuint vert, GLuint frag, const char* tag) {
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vert); glAttachShader(prog, frag);
    glLinkProgram(prog);
    glDetachShader(prog, vert); glDeleteShader(vert);
    glDetachShader(prog, frag); glDeleteShader(frag);
    GLint ok = 0; glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) { fprintf(stderr, "LayerManager [%s] link failed\n", tag);
               glDeleteProgram(prog); return 0; }
    return prog;
}

// ── build programs ────────────────────────────────────────────────────────────

bool LayerManager::buildCompProgram() {
    GLuint v = compileInline(GL_VERTEX_SHADER,   kFullscreenVert, "comp.vert");
    GLuint f = compileInline(GL_FRAGMENT_SHADER, kCompFrag,       "comp.frag");
    if (!v || !f) return false;
    m_compProgram = linkProgram(v, f, "comp");
    return m_compProgram != 0;
}

bool LayerManager::buildFXProgram() {
    GLuint v = compileInline(GL_VERTEX_SHADER,   kFullscreenVert, "fx.vert");
    GLuint f = compileInline(GL_FRAGMENT_SHADER, kFXFrag,         "fx.frag");
    if (!v || !f) return false;
    m_fxProgram = linkProgram(v, f, "fx");
    return m_fxProgram != 0;
}

// ── lifecycle ─────────────────────────────────────────────────────────────────

bool LayerManager::init(int width, int height) {
    m_width = width; m_height = height;
    GL_CHECK(glGenVertexArrays(1, &m_compVao));
    m_fxVao = m_compVao;                    // same null VAO works for both passes
    if (!buildCompProgram()) return false;
    if (!buildFXProgram())   return false;
    return m_fxRT.init(width, height);      // composite target for post-proc
}

void LayerManager::update(float time, float delta, bool beat, double bpm) {
    for (auto& e : m_layers)
        if (e.layer->enabled())
            e.layer->update(time, delta, beat, bpm);
}

void LayerManager::render(const MasterFX& fx) {
    // ── Pass 1: composite all layers into m_fxRT ──────────────────────────
    m_fxRT.bind();
    GL_CHECK(glClearColor(0.0f, 0.0f, 0.0f, 1.0f));
    GL_CHECK(glClear(GL_COLOR_BUFFER_BIT));
    GL_CHECK(glEnable(GL_BLEND));

    GLint locTex = glGetUniformLocation(m_compProgram, "uTex");
    GLint locOp  = glGetUniformLocation(m_compProgram, "uOpacity");

    for (auto& e : m_layers) {
        if (!e.layer->enabled()) continue;

        // Render layer into its private RT (changes active program + VAO)
        e.rt.bind();
        GL_CHECK(glClearColor(0.0f, 0.0f, 0.0f, 0.0f));
        GL_CHECK(glClear(GL_COLOR_BUFFER_BIT));
        e.layer->render();

        // Blit into composite target — restore comp program + VAO first
        m_fxRT.bind();
        GL_CHECK(glViewport(0, 0, m_width, m_height));
        GL_CHECK(glUseProgram(m_compProgram));
        GL_CHECK(glBindVertexArray(m_compVao));
        applyBlend(e.layer->blendMode());
        e.rt.bindTexture(0);
        if (locTex >= 0) GL_CHECK(glUniform1i(locTex, 0));
        if (locOp  >= 0) GL_CHECK(glUniform1f(locOp, e.layer->opacity()));
        GL_CHECK(glDrawArrays(GL_TRIANGLES, 0, 3));
    }

    GL_CHECK(glBindVertexArray(0));
    GL_CHECK(glUseProgram(0));
    resetBlend();

    // ── Pass 2: master FX onto default framebuffer ────────────────────────
    GL_CHECK(glBindFramebuffer(GL_FRAMEBUFFER, 0));
    GL_CHECK(glViewport(0, 0, m_width, m_height));
    GL_CHECK(glDisable(GL_BLEND));

    GL_CHECK(glUseProgram(m_fxProgram));
    auto setF = [&](const char* n, float v) {
        GLint l = glGetUniformLocation(m_fxProgram, n);
        if (l >= 0) GL_CHECK(glUniform1f(l, v));
    };
    auto setI = [&](const char* n, int v) {
        GLint l = glGetUniformLocation(m_fxProgram, n);
        if (l >= 0) GL_CHECK(glUniform1i(l, v));
    };
    m_fxRT.bindTexture(0);
    setI("uTex",        0);
    setF("uBrightness", fx.brightness);
    setF("uContrast",   fx.contrast);
    setF("uSaturation", fx.saturation);
    setF("uHue",        fx.hue);
    setF("uGlow",       fx.glow);
    setF("uVignette",   fx.vignette);
    setF("uAberration", fx.aberration);

    GL_CHECK(glBindVertexArray(m_compVao));
    GL_CHECK(glDrawArrays(GL_TRIANGLES, 0, 3));
    GL_CHECK(glBindVertexArray(0));
    GL_CHECK(glUseProgram(0));
}

void LayerManager::renderGUI() {
    for (int i = 0; i < (int)m_layers.size(); ++i)
        m_layers[i].layer->renderGUI();
}

void LayerManager::resize(int width, int height) {
    m_width = width; m_height = height;
    m_fxRT.init(width, height);
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
    if (m_fxProgram)   { GL_CHECK(glDeleteProgram(m_fxProgram));   m_fxProgram  = 0; }
    m_fxVao = 0;
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
        std::rotate(m_layers.begin() + from, m_layers.begin() + from + 1,
                    m_layers.begin() + to + 1);
    else
        std::rotate(m_layers.begin() + to,   m_layers.begin() + from,
                    m_layers.begin() + from + 1);
}

ILayer* LayerManager::getLayer(int idx) {
    if (idx < 0 || idx >= (int)m_layers.size()) return nullptr;
    return m_layers[idx].layer.get();
}

// ── blend modes ───────────────────────────────────────────────────────────────

void LayerManager::applyBlend(int mode) {
    switch (mode) {
    case ILayer::BLEND_ADDITIVE:
        GL_CHECK(glBlendFunc(GL_ONE, GL_ONE)); break;
    case ILayer::BLEND_NORMAL:
        GL_CHECK(glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)); break;
    case ILayer::BLEND_SCREEN:
        GL_CHECK(glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_COLOR)); break;
    case ILayer::BLEND_MULTIPLY:
        GL_CHECK(glBlendFunc(GL_DST_COLOR, GL_ZERO)); break;
    default:
        GL_CHECK(glBlendFunc(GL_ONE, GL_ONE)); break;
    }
}

void LayerManager::resetBlend() {
    GL_CHECK(glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
    GL_CHECK(glDisable(GL_BLEND));
}
