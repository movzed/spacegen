#pragma once
// GlitchEffect — VHS-style screen-space corruption, single fullscreen pass.
//
// Five independent sub-effects, each with its own activation threshold so
// the operator can ramp from "subtle" to "full collapse" with one master
// amount. Sub-effects are toggleable independently.
//
//   1. Displacement bands — random per-band shift in X (or Y)
//   2. RGB swap           — R↔B in random rectangular regions
//   3. Block dropout      — small rectangles output black / noise
//   4. Tear line          — one tall thin slice hard-shifted (ripped tape)
//   5. Color flash        — full-screen 1-2 frame color pulse
//
// Master amount is the sum of:
//   - static `masterAmount` slider
//   - optional ModulatorBank slot (depth-multiplied)
//   - optional BPM-locked beat envelope (exp-decay attack on each beat)
//
// Pseudo-randomness is quantised to BPM beats (or frames if BPM=0), so the
// pattern is stable within a beat and re-rolls on the next downbeat. This
// is the "snap to grid" behaviour VJs expect.

#include "Layer.h"
#include <Metal/Metal.hpp>

namespace MTL {
class RenderPipelineState;
class Library;
}

namespace spacegen {

class GlitchEffect : public ILayer {
public:
    GlitchEffect();
    ~GlitchEffect() override;

    LayerKind   kind()     const override { return LayerKind::Effect; }
    const char* typeName() const override { return "Glitch"; }

    void render(RenderContext& ctx) override;
    void drawInspector() override;

    // ---- Master amount ----------------------------------------------------
    float masterAmount = 0.0f;       // 0..1

    // BPM-locked envelope. bpm = 0 disables the schedule (continuous mode).
    float bpm           = 128.0f;    // 0 = disabled
    float beatStrength  = 0.6f;      // peak amount added at each downbeat
    float beatEnvDecay  = 8.0f;      // exp decay rate within a beat (1..32)

    // Optional ModulatorBank binding for the amount.
    int   amountModSlot  = 0;        // 0 = unbound, 1..8 = bank slot
    float amountModDepth = 0.5f;

    // ---- Common knobs -----------------------------------------------------
    uint32_t seed             = 1337;
    int      bandThicknessPx  = 8;   // 2..64 — pixel stride for displacement bands

    // ---- Sub-effects ------------------------------------------------------
    struct SubEffect {
        bool  enabled   = true;
        float threshold = 0.0f;      // fires when effectiveAmount > threshold
    };
    SubEffect bands;   // defaults set in ctor
    SubEffect rgbSwap;
    SubEffect dropout;
    SubEffect tear;
    SubEffect flash;

    // Color of the full-screen flash pulse (sub-effect 5).
    glm::vec3 flashColor = glm::vec3(1.0f, 0.10f, 0.30f);

private:
    void  buildPipeline(MTL::Device* device, MTL::PixelFormat fmt);
    float computeEffectiveAmount(const RenderContext& ctx) const;

    MTL::RenderPipelineState* pipeline_ = nullptr;
    MTL::PixelFormat          builtFmt_ = (MTL::PixelFormat)0;
};

} // namespace spacegen
