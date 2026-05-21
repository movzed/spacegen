#pragma once
// SyphonInputLayer — receives a live video texture from an external Syphon
// server (Resolume Arena, MadMapper, Touch Designer, OBS, etc.) and feeds it
// to the structure shader as a baseColor override. The texture maps onto the
// mesh via its glTF UVs, respecting the 3D geometry of the building.
//
// The implementation lives in SyphonInputLayer.mm (ObjC++) because the
// Syphon SDK is Objective-C. From C++ we only see an opaque impl pointer
// and a current Metal texture handle.

#include "Layer.h"

#include <string>
#include <vector>

namespace MTL { class Texture; }

namespace spacegen {

class SyphonInputLayer : public ILayer {
public:
    SyphonInputLayer();
    ~SyphonInputLayer() override;

    SyphonInputLayer(const SyphonInputLayer&)            = delete;
    SyphonInputLayer& operator=(const SyphonInputLayer&) = delete;

    LayerKind   kind()     const override { return LayerKind::Generator; }
    const char* typeName() const override { return "Syphon Input"; }
    void        render(RenderContext& ctx) override;   // pulls a new frame
    void        drawInspector() override;

    // ---- Inspector controls ----
    // Mix amount: how much the Syphon texture overrides the structure's
    // baseColor. 0 = no effect, 1 = pure Syphon (still gets PBR lighting).
    float mix = 1.0f;
    // Tint multiplied with the Syphon sample (vec3). Default white.
    glm::vec3 tint = glm::vec3(1.0f);

    // Returns the last received Metal texture (may be null if no frame yet).
    MTL::Texture* currentTexture() const;

    // Returns the human-readable name of the currently connected server, or
    // empty string if no connection.
    std::string currentServerName() const;

private:
    // Opaque ObjC++ impl (SyphonClientWrapper*). Lifecycle managed by
    // SyphonInputLayer.mm.
    void* impl_ = nullptr;
};

} // namespace spacegen
