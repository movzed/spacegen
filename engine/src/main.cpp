#include "core/gl.h"
#include "core/Window.h"
#include "core/BPMClock.h"
#include "core/LayerManager.h"
#include "layers/PlasmaWaveLayer.h"
#include "gui/GUI.h"
#include <cstdio>
#include <memory>

int main() {
    Window win(1280, 720, "SpaceGen");
    if (!win.init()) { fprintf(stderr, "Window init failed\n"); return 1; }

    BPMClock    clock;
    LayerManager layers;
    GUI          gui;

    if (!layers.init(win.width(), win.height())) {
        fprintf(stderr, "LayerManager init failed\n"); return 1;
    }

    if (!gui.init(win.handle())) {
        fprintf(stderr, "GUI init failed\n"); return 1;
    }

    // Resize callback wires window → layers
    win.setResizeCallback([&](int w, int h) {
        layers.resize(w, h);
    });

    // Phase 1: single PlasmaWave layer
    auto plasma = std::make_unique<PlasmaWaveLayer>();
    plasma->init(win.width(), win.height());
    layers.addLayer(std::move(plasma));

    // Main loop
    while (!win.shouldClose() && !gui.wantsQuit()) {
        win.pollEvents();

        bool beat = clock.update();
        float time  = static_cast<float>(clock.elapsed());
        float delta = clock.delta();
        double bpm  = clock.bpm();

        layers.update(time, delta, beat, bpm);

        // Clear default framebuffer
        GL_CHECK(glBindFramebuffer(GL_FRAMEBUFFER, 0));
        GL_CHECK(glViewport(0, 0, win.width(), win.height()));
        GL_CHECK(glClearColor(0.0f, 0.0f, 0.0f, 1.0f));
        GL_CHECK(glClear(GL_COLOR_BUFFER_BIT));

        layers.render(gui.masterFX());

        gui.beginFrame();
        gui.render(clock, layers);
        gui.endFrame();

        win.swapBuffers();
    }

    gui.shutdown();
    layers.shutdown();
    return 0;
}
