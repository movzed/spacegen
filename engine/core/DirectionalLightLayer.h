#pragma once
// DirectionalLightLayer — an infinitely-far light (sun / fill light). Has
// only direction + color + intensity (no position, no cone). Consumed by
// the structure pass.

#include "Layer.h"
#include "LFO.h"

namespace spacegen {

class DirectionalLightLayer : public ILayer {
public:
    DirectionalLightLayer();

    LayerKind   kind()     const override { return LayerKind::Generator; }
    const char* typeName() const override { return "Directional Light"; }
    void        render(RenderContext& /*ctx*/) override {}  // consumed by StructureLayer
    void        drawInspector() override;

    // World-space direction the light is travelling (TOWARDS the scene).
    // Stored as pan/tilt so the UI is intuitive; converted to a vec3 at
    // render time.
    float       panDeg  = 30.0f;   // yaw around world Z
    float       tiltDeg = -45.0f;  // pitch from horizontal (negative = down)

    glm::vec3   color     = glm::vec3(1.0f, 0.95f, 0.88f);
    float       intensity = 1.0f;

    // Optional LFO modulators
    LFO         panLFO;
    LFO         tiltLFO;
    LFO         intensityLFO;

    // Computes the final world-space direction (normalized) given current
    // pan/tilt + LFO offsets at time t.
    glm::vec3 directionAtTime(double t) const;
    float     intensityAtTime(double t) const;
};

} // namespace spacegen
