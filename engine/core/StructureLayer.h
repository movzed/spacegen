#pragma once
// StructureLayer — the PBR-lit structure mesh (always present, always first).
// Wraps the existing structure pass + its directional light. v1 holds the
// material + light params directly; M3-C wraps them in Parameter objects.

#include "Layer.h"

namespace spacegen {

class StructureLayer : public ILayer {
public:
    StructureLayer();

    LayerKind   kind()     const override { return LayerKind::Generator; }
    const char* typeName() const override { return "Structure"; }
    void        render(RenderContext& ctx) override;
    void        drawInspector() override;

    // Material (read by MetalRenderer::renderStructureMeshes)
    glm::vec3 baseColor      = glm::vec3(0.72f);
    float     roughness      = 0.55f;
    float     metallic       = 0.0f;
    float     ambient        = 0.04f;

    // Directional light (also baked into the structure shader for v1)
    glm::vec3 lightDirection = glm::vec3(0.4f, -0.6f, -0.6f);
    glm::vec3 lightColor     = glm::vec3(1.0f, 0.96f, 0.92f);
    float     lightIntensity = 1.6f;
};

} // namespace spacegen
