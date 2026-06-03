#pragma once
// ChromaticAberrationEffect — radial RGB channel split post-process.
//
// Reads ctx.colorTarget as a texture, samples R/G/B at slightly different
// scales around `center`, writes the result to the renderer's ping-pong
// output target. Wavelength-correct ordering: red OUTSIDE, blue INSIDE
// (Cauchy 1836; n(λ) decreases with λ).
//
// Pattern modes:
//   Radial     — uniform stretch with distance (default)
//   Linear     — purely horizontal smear (VHS-like)
//   Barrel     — positive lens distortion (offset ~ dist²)
//   Pincushion — negative lens distortion (edges pulled in)
//
// Single fullscreen-quad pass. No mesh data required.

#include "../../../core/Layer.h"

namespace MTL {
class RenderPipelineState;
class Library;
}

namespace spacegen {

class ChromaticAberrationEffect : public ILayer {
public:
    ChromaticAberrationEffect();
    ~ChromaticAberrationEffect() override;

    LayerKind   kind()     const override { return LayerKind::Effect; }
    const char* typeName() const override { return "Chromatic Aberration"; }

    void render(RenderContext& ctx) override;
    void drawInspector() override;

    // ---- Operator-facing parameters ----
    enum class Pattern : int {
        Radial     = 0,
        Linear     = 1,
        Barrel     = 2,
        Pincushion = 3,
    };

    float     strength = 0.012f;                 // 0..0.1 — max channel offset
    glm::vec2 center   = glm::vec2(0.5f, 0.5f);  // UV space (0..1)
    Pattern   pattern  = Pattern::Radial;

    // ---- Modulator-bank binding (optional) ----
    int   strengthModSlot  = 0;     // 0 = unbound, 1..8 = bank slot
    float strengthModDepth = 0.02f; // multiplies the LFO output (-1..+1)

private:
    void buildPipeline(MTL::Device* device, MTL::PixelFormat fmt);

    MTL::RenderPipelineState* pipeline_   = nullptr;
    MTL::PixelFormat          builtFmt_   = (MTL::PixelFormat)0;
};

} // namespace spacegen
