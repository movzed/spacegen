#pragma once
// BeamLayer — one world-space volumetric beam, raymarched, additive.
// Instantiable N times in the Bus (the operator can add multiple beams
// of different colors / origins via the Layer Rack UI).

#include "Layer.h"

namespace spacegen {

class BeamLayer : public ILayer {
public:
    BeamLayer();

    LayerKind   kind()     const override { return LayerKind::Generator; }
    const char* typeName() const override { return "VolumetricBeam"; }
    void        render(RenderContext& ctx) override;
    void        drawInspector() override;

    glm::vec3 origin     = glm::vec3(0.0f, 3.0f, 4.0f);
    glm::vec3 direction  = glm::vec3(0.0f, -0.8f, -0.6f);
    glm::vec3 color      = glm::vec3(0.30f, 0.55f, 1.00f);
    float     intensity  = 1.2f;
    float     range      = 14.0f;
    float     coneDeg    = 14.0f;
    float     falloff    = 1.8f;
    int       steps      = 32;
};

} // namespace spacegen
