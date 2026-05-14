#pragma once
#include "gl.h"

// Wraps an OpenGL FBO with a colour texture attachment.
// Used for per-layer offscreen rendering and ping-pong feedback effects.
class RenderTarget {
public:
    RenderTarget()  = default;
    ~RenderTarget();

    RenderTarget(RenderTarget&& o) noexcept
        : m_fbo(o.m_fbo), m_tex(o.m_tex), m_rbo(o.m_rbo), m_w(o.m_w), m_h(o.m_h)
        { o.m_fbo = o.m_tex = o.m_rbo = 0; }

    RenderTarget& operator=(RenderTarget&& o) noexcept {
        if (this != &o) { release(); m_fbo=o.m_fbo; m_tex=o.m_tex; m_rbo=o.m_rbo;
                          m_w=o.m_w; m_h=o.m_h; o.m_fbo=o.m_tex=o.m_rbo=0; }
        return *this;
    }

    RenderTarget(const RenderTarget&)            = delete;
    RenderTarget& operator=(const RenderTarget&) = delete;

    // Allocate / reallocate GPU resources. Safe to call on resize.
    bool init(int width, int height);

    void bind()   const;  // glBindFramebuffer(GL_FRAMEBUFFER, m_fbo)
    void unbind() const;  // glBindFramebuffer(GL_FRAMEBUFFER, 0)

    // Bind the colour texture to a texture unit for sampling downstream.
    void bindTexture(int unit = 0) const;

    GLuint texture() const { return m_tex; }
    int    width()   const { return m_w; }
    int    height()  const { return m_h; }
    bool   valid()   const { return m_fbo != 0; }

private:
    void release();

    GLuint m_fbo = 0;
    GLuint m_tex = 0;
    GLuint m_rbo = 0;   // depth/stencil renderbuffer
    int    m_w   = 0;
    int    m_h   = 0;
};

// ── Ping-pong pair ─────────────────────────────────────────────────────────
// Two RenderTargets swapped each frame — used by feedback and reaction-diffusion layers.
class PingPong {
public:
    bool init(int w, int h);
    void resize(int w, int h);

    RenderTarget& write() { return m_rt[m_idx]; }
    RenderTarget& read()  { return m_rt[1 - m_idx]; }
    void swap()           { m_idx = 1 - m_idx; }

private:
    RenderTarget m_rt[2];
    int          m_idx = 0;
};
