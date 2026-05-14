#pragma once
#include "gl.h"
#include <GLFW/glfw3.h>
#include <functional>
#include <string>

class Window {
public:
    Window(int w, int h, const std::string& title);
    ~Window();

    bool        init();
    void        pollEvents();
    void        swapBuffers();
    bool        shouldClose() const;
    void        toggleFullscreen();

    int         width()  const { return m_width; }
    int         height() const { return m_height; }
    float       aspect() const { return static_cast<float>(m_width) / static_cast<float>(m_height); }
    GLFWwindow* handle() const { return m_handle; }

    void setResizeCallback(std::function<void(int,int)> cb) { m_onResize = std::move(cb); }

private:
    static void cbFramebuffer(GLFWwindow*, int w, int h);
    static void cbKey(GLFWwindow*, int key, int scancode, int action, int mods);

    GLFWwindow* m_handle   = nullptr;
    int         m_width, m_height;
    int         m_savedX = 0, m_savedY = 0, m_savedW, m_savedH;
    bool        m_fullscreen = false;
    std::string m_title;

    std::function<void(int,int)> m_onResize;
};
