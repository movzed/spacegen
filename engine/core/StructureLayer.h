#pragma once
// StructureLayer — the PBR-shaded structure mesh. Holds only material
// parameters; ALL lights are now separate layers (DirectionalLightLayer /
// BeamLayer). StructureLayer's render() queries the bus for those, packs
// them into uniforms, and dispatches the structure pass.

#include "Layer.h"

namespace spacegen {

class StructureLayer : public ILayer {
public:
    StructureLayer();

    LayerKind   kind()     const override { return LayerKind::Generator; }
    const char* typeName() const override { return "Structure"; }
    void        render(RenderContext& ctx) override;
    void        drawInspector() override;

    // PBR material (read by MetalRenderer::renderStructureMeshes).
    glm::vec3 baseColor = glm::vec3(0.72f);
    float     roughness = 0.55f;
    float     metallic  = 0.0f;
    float     ambient   = 0.10f;   // small fill so unlit structure isn't pitch black
};

} // namespace spacegen
