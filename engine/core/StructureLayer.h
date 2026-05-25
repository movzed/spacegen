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

    // Stretch heatmap diagnostic. When enabled, the structure renders as a
    // viridis-like heatmap of per-fragment UV distortion instead of the
    // normal material / Syphon composition. Driven by the UV Analysis panel.
    //   stretchMetric: 0 = stretch ratio (ideal 1.0), 1 = symmetric Dirichlet
    //                  energy density (ideal 4.0).
    //   stretchUV:     0 = measure UV0 (PRT_UVW), 1 = measure UV1 (atlas).
    bool      showStretchHeatmap = false;
    int       stretchMetric      = 0;
    int       stretchUV          = 1;

    // ---- 3D mesh effects (vertex displacement) ----
    // displaceAmount: meters of outward displacement along the vertex normal,
    //   modulated by hash-noise. 0 = no effect. Operator can animate via
    //   binding to a ModulatorBank LFO (slot 0 = unbound).
    // displaceScale: spatial frequency of the noise (larger = finer detail).
    // twistAmount: shear that rotates vertices around the Z axis as a
    //   function of their height — gives a "tornado" / "unraveling" feel.
    float     displaceAmount = 0.0f;
    float     displaceScale  = 1.5f;
    float     twistAmount    = 0.0f;
    int       displaceModSlot   = 0;
    float     displaceModDepth  = 1.0f;
    int       twistModSlot      = 0;
    float     twistModDepth     = 0.5f;
};

} // namespace spacegen
