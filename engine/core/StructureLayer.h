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
    // Ambient fill is a separate layer (AmbientLightLayer) — add one to the
    // bus when you want a fill floor on the structure. No more hidden
    // ambient in the material.

    // Output mode:
    //   false (default): structure renders fully — base color + ambient +
    //                    lighting. Alpha = 1. Operator sees the relief.
    //   true:            structure base color and ambient are suppressed.
    //                    Output is only the lighting contribution, with
    //                    alpha proportional to brightness. Perfect for
    //                    compositing in Resolume on top of Blender plates.
    bool      emitLightsOnly = false;
};

} // namespace spacegen
