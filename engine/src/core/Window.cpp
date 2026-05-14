#include "Window.h"
#include <cstdio>

Window::Window(int w, int h, const std::string& title)
    : m_width(w), m_height(h), m_savedW(w), m_savedH(h), m_title(title) {}

Window::~Window() {
    if (m_handle) glfwDestroyWindow(m_handle);
    glfwTerminate();
}

bool Window::init() {
    glfwSetErrorCallback([](int code, const char* msg) {
        fprintf(stderr, "GLFW error %d: %s\n", code, msg);
    });

    if (!glfwInit()) { fprintf(stderr, "glfwInit failed\n"); return false; }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
    glfwWindowHint(GLFW_OPENGL_PROFILE,        GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    glfwWindowHint(GLFW_SAMPLES, 0);

    m_handle = glfwCreateWindow(m_width, m_height, m_title.c_str(), nullptr, nullptr);
    if (!m_handle) { fprintf(stderr, "glfwCreateWindow failed\n"); glfwTerminate(); return false; }

    glfwSetWindowUserPointer(m_handle, this);
    glfwMakeContextCurrent(m_handle);
    glfwSwapInterval(1);

    glfwSetFramebufferSizeCallback(m_handle, cbFramebuffer);
    glfwSetKeyCallback(m_handle, cbKey);

    fprintf(stdout, "OpenGL  %s\nGLSL    %s\nGPU     %s\n",
        glGetString(GL_VERSION),
        glGetString(GL_SHADING_LANGUAGE_VERSION),
        glGetString(GL_RENDERER));

    return true;
}

void Window::pollEvents()    { glfwPollEvents(); }
void Window::swapBuffers()   { glfwSwapBuffers(m_handle); }
bool Window::shouldClose() const { return glfwWindowShouldClose(m_handle) != 0; }

void Window::toggleFullscreen() {
    m_fullscreen = !m_fullscreen;
    if (m_fullscreen) {
        glfwGetWindowPos (m_handle, &m_savedX, &m_savedY);
        glfwGetWindowSize(m_handle, &m_savedW, &m_savedH);
        GLFWmonitor*       mon  = glfwGetPrimaryMonitor();
        const GLFWvidmode* mode = glfwGetVideoMode(mon);
        glfwSetWindowMonitor(m_handle, mon, 0, 0, mode->width, mode->height, mode->refreshRate);
    } else {
        glfwSetWindowMonitor(m_handle, nullptr, m_savedX, m_savedY, m_savedW, m_savedH, 0);
    }
}

void Window::cbFramebuffer(GLFWwindow* w, int width, int height) {
    auto* self = static_cast<Window*>(glfwGetWindowUserPointer(w));
    self->m_width  = width;
    self->m_height = height;
    GL_CHECK(glViewport(0, 0, width, height));
    if (self->m_onResize) self->m_onResize(width, height);
}

void Window::cbKey(GLFWwindow* w, int key, int /*scan*/, int action, int mods) {
    auto* self = static_cast<Window*>(glfwGetWindowUserPointer(w));
    if (action != GLFW_PRESS) return;
    // Cmd+F → fullscreen toggle
    if (key == GLFW_KEY_F && (mods & GLFW_MOD_SUPER)) self->toggleFullscreen();
    // Escape → quit
    if (key == GLFW_KEY_ESCAPE) glfwSetWindowShouldClose(w, GLFW_TRUE);
}
