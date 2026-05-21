#pragma once
// AmbientLightLayer — global directionless fill light. Multiplies the
// structure's base color uniformly. Several can be stacked for tinted
// fills (warm + cool, for example). Disable for a pitch-black baseline
// where only spot lights illuminate the structure.

#include "Layer.h"

namespace spacegen {

class AmbientLightLayer : public ILayer {
public:
    AmbientLightLayer();

    LayerKind   kind()     const override { return LayerKind::Generator; }
    const char* typeName() const override { return "Ambient Fill"; }
    void        render(RenderContext& /*ctx*/) override {}  // consumed by StructureLayer
    void        drawInspector() override;

    glm::vec3 color     = glm::vec3(1.0f);   // tint (white = pure base color)
    float     intensity = 0.02f;             // multiplier
};

} // namespace spacegen
