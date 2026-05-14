#include "RenderTarget.h"
#include <cstdio>

RenderTarget::~RenderTarget() { release(); }

void RenderTarget::release() {
    if (m_fbo) { glDeleteFramebuffers(1,  &m_fbo); m_fbo = 0; }
    if (m_tex) { glDeleteTextures(1,      &m_tex); m_tex = 0; }
    if (m_rbo) { glDeleteRenderbuffers(1, &m_rbo); m_rbo = 0; }
}

bool RenderTarget::init(int width, int height) {
    release();
    m_w = width; m_h = height;

    // Colour texture
    GL_CHECK(glGenTextures(1, &m_tex));
    GL_CHECK(glBindTexture(GL_TEXTURE_2D, m_tex));
    GL_CHECK(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_FLOAT, nullptr));
    GL_CHECK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
    GL_CHECK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
    GL_CHECK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
    GL_CHECK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));

    // FBO
    GL_CHECK(glGenFramebuffers(1, &m_fbo));
    GL_CHECK(glBindFramebuffer(GL_FRAMEBUFFER, m_fbo));
    GL_CHECK(glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_tex, 0));

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "RenderTarget: incomplete FBO (0x%x)\n", status);
        release(); return false;
    }

    GL_CHECK(glBindFramebuffer(GL_FRAMEBUFFER, 0));
    GL_CHECK(glBindTexture(GL_TEXTURE_2D, 0));
    return true;
}

void RenderTarget::bind()   const { GL_CHECK(glBindFramebuffer(GL_FRAMEBUFFER, m_fbo)); GL_CHECK(glViewport(0,0,m_w,m_h)); }
void RenderTarget::unbind() const { GL_CHECK(glBindFramebuffer(GL_FRAMEBUFFER, 0)); }

void RenderTarget::bindTexture(int unit) const {
    GL_CHECK(glActiveTexture(GL_TEXTURE0 + unit));
    GL_CHECK(glBindTexture(GL_TEXTURE_2D, m_tex));
}

// ── PingPong ───────────────────────────────────────────────────────────────

bool PingPong::init(int w, int h) {
    return m_rt[0].init(w, h) && m_rt[1].init(w, h);
}

void PingPong::resize(int w, int h) {
    m_rt[0].init(w, h); m_rt[1].init(w, h);
}
