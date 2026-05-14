#pragma once
#include "ShaderLayer.h"
#include "imgui.h"

class PlasmaWaveLayer : public ShaderLayer {
public:
    explicit PlasmaWaveLayer() : ShaderLayer("PlasmaWave") {}

    bool init(int width, int height) override {
        if (!ShaderLayer::init(width, height)) return false;
        setShaderPaths("shaders/common/fullscreen.vert",
                       "shaders/layers/plasma.frag");
        return true;
    }

protected:
    void onSetUniforms(float time, float delta, bool beat, double /*bpm*/) override {
        if (beat) m_beatFlash = 1.0f;
        m_beatFlash = std::max(0.0f, m_beatFlash - delta * 4.0f);

        m_shader.set("uTime",       time);
        m_shader.set("uSpeed",      m_speed);
        m_shader.set("uScale",      m_scale);
        m_shader.set("uContrast",   m_contrast);
        m_shader.set("uBeatFlash",  m_beatFlash);
        m_shader.set("uColor1",     m_color1);
        m_shader.set("uColor2",     m_color2);
        m_shader.set("uColor3",     m_color3);
    }

    void onRenderGUI() override {
        ImGui::SliderFloat("Speed",    &m_speed,    0.1f, 5.0f);
        ImGui::SliderFloat("Scale",    &m_scale,    0.5f, 8.0f);
        ImGui::SliderFloat("Contrast", &m_contrast, 0.5f, 3.0f);
        ImGui::ColorEdit3("Color 1",  &m_color1[0]);
        ImGui::ColorEdit3("Color 2",  &m_color2[0]);
        ImGui::ColorEdit3("Color 3",  &m_color3[0]);
    }

private:
    float     m_speed     = 1.0f;
    float     m_scale     = 3.0f;
    float     m_contrast  = 1.2f;
    float     m_beatFlash = 0.0f;
    glm::vec3 m_color1    = {1.0f, 0.3f, 0.8f};
    glm::vec3 m_color2    = {0.1f, 0.6f, 1.0f};
    glm::vec3 m_color3    = {0.8f, 1.0f, 0.2f};
};
