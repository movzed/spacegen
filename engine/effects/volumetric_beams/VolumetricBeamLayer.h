#pragma once
// VolumetricBeamLayer — raymarched "cones of light in air" companion to
// BeamLayer. POST-PROCESS pass that reads the StructureLayer's depth buffer,
// walks the bus for spotlights, packs them into a uniform array (the same
// 32-fixture cap as the structure pbr shader), and dispatches a fullscreen
// quad that single-scatters through homogeneous participating media using
// the Henyey-Greenstein phase function.
//
// Render order: this layer must run AFTER StructureLayer (so it can read
// the depth + color attachments). Operationally that means the operator
// drops a VolumetricBeamLayer at any position above StructureLayer in the
// bus rack. The renderer does the rest.
//
// Add it once; it covers every BeamLayer in the bus. If the operator wants
// some spots to emit volumetrically and others not, they can set the
// per-fixture multiplier or simply disable individual BeamLayers — the
// volumetric mirrors whatever's lit.

#include "Layer.h"

#include <array>
#include <string>

namespace spacegen {

class VolumetricBeamLayer : public ILayer {
public:
    VolumetricBeamLayer();

    LayerKind   kind()     const override { return LayerKind::Effect; }
    const char* typeName() const override { return "Volumetric Beams"; }

    void render(RenderContext& ctx) override;
    void drawInspector() override;

    // ---- Master parameters --------------------------------------------------

    // Scattering coefficient. Real concert hazers map to roughly 0.005-0.03;
    // we expose 0..0.1 so the operator can push to "mystical fog" territory
    // for builds and stings. 0 disables the layer (early-out, no perf cost
    // beyond the early-return).
    float density = 0.020f;

    // Henyey-Greenstein anisotropy. Per the README: 0 = isotropic, +0.7 is
    // theatre haze, the slider is clamped to (-0.99..0.99) at upload time
    // to avoid the singularity at g=±1.
    float anisotropy = 0.70f;

    // Number of raymarch samples per fragment. Higher = smoother cones at
    // higher perf cost. See README for budget table. Clamped to [4, 128]
    // in the inspector; 48 default lands ~1.2 ms @ 1080p on M1 Max.
    int sampleCount = 48;

    // Jitter strength in [0, 1]. 0 = banding visible, 1 = full ±0.5-step
    // dither, smoothest at the cost of a slight grainy shimmer. 0.85 is
    // a good default — banding broken with minimal perceptual grain.
    float jitterStrength = 0.85f;

    // Beer-Lambert extinction along the view ray. When true, deeper samples
    // see less of the camera's view because of self-absorption (more
    // physically motivated). Off by default for the cleaner stage look
    // and slightly cheaper math.
    bool  beerLambertExtinction = false;

    // Color multiplier. Useful for desaturating the cones (e.g. dust haze
    // that should look slightly less saturated than the source lights),
    // or for a stylized tint. White = unchanged.
    glm::vec3 tint = glm::vec3(1.0f);

    // Optional max cone length cap. When < 0 the spotlight's `range` is
    // used unchanged. When >= 0 the integrated path length is clamped to
    // this value (in meters), useful for fixtures with very long `range`
    // but where the operator only wants the cone visible for the first
    // few meters.
    float maxConeLength = -1.0f;

    // ---- Per-spotlight contribution multipliers ----------------------------
    // Indexed by global fixture index (BeamLayer rigs are flattened to N
    // fixtures by `fixturePositions(ctx)`; we lay them out in bus order,
    // matching the structure pass). Allows boosting "the laser-look beam"
    // or muting a chase fixture from the volumetric pass without touching
    // its surface contribution.
    //
    // 32 slots == kMaxSpots in the renderer (see MetalRenderer.cpp). Slots
    // beyond the live fixture count are inert.
    static constexpr int kMaxFixtures = 32;
    std::array<float, kMaxFixtures> perFixtureMul = []() {
        std::array<float, kMaxFixtures> a{};
        a.fill(1.0f);
        return a;
    }();
    // Cached pretty name per fixture, refreshed each frame from the BeamLayer
    // owners ("Spot.fixture[2]"). For inspector UX only.
    std::array<std::string, kMaxFixtures> fixtureLabels{};
    int activeFixtureCount = 0;
};

} // namespace spacegen
