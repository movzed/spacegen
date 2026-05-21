#pragma once
// BeamLayer — a spot light (projector-style) that illuminates the structure
// in the structure pass. Despite the name, this is NOT a volumetric beam:
// in projection mapping the light is the projector itself, so we don't see
// the cone in air — we only see its illumination on the surface.
//
// The light data is consumed by StructureLayer::render() which queries the
// bus for all enabled BeamLayers, packs them into the structure shader's
// spot-light array, and renders the structure once with all spots applied.
// Therefore BeamLayer::render() itself is a no-op.

#include "Layer.h"

namespace spacegen {

class BeamLayer : public ILayer {
public:
    BeamLayer();

    LayerKind   kind()     const override { return LayerKind::Generator; }
    const char* typeName() const override { return "Spot Light"; }
    void        render(RenderContext& /*ctx*/) override {}  // consumed by StructureLayer
    void        drawInspector() override;

    // Spot-light params (world space).
    glm::vec3 origin    = glm::vec3(0.0f, -6.0f, 1.0f);     // overwritten on creation
    glm::vec3 direction = glm::vec3(0.0f, 1.0f, 0.0f);      // overwritten on creation
    glm::vec3 color     = glm::vec3(1.0f, 1.0f, 1.0f);
    float     intensity = 5.0f;
    float     range     = 100.0f;                            // meters; large -> no falloff
    float     innerDeg  = 8.0f;                              // full-intensity half-angle
    float     outerDeg  = 14.0f;                             // edge half-angle (soft falloff)

    // Convenience: snap the spot to the scene camera. Used by the "+ Beam"
    // button so new lights start as projector lights from the camera.
    bool      followCamera = false;
};

} // namespace spacegen
