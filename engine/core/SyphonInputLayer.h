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

    // How the video is mapped onto the structure. For projection-mapping work
    // the default (Projector) behaves like a real video projector: the image
    // is projected from the scene camera's POV, independent of the mesh UVs.
    // This is what you want for buildings where Blender unwraps are partial
    // or non-existent. UV mode (legacy) needs a proper unwrap on every face
    // to avoid degenerate-UV "solid color" patches. Triplanar tiles the
    // image in world space using the surface normal as the blend basis —
    // useful for textures/patterns rather than narrative video.
    enum class ProjMode : int {
        Projector = 0,   // NDC of scene camera (recommended for v1)
        Triplanar = 1,   // world-pos blended by |N| — UV-independent
        UV        = 2,   // mesh UV0 — depends on Blender unwrap quality
        Auto      = 3,   // UV where the unwrap is valid, Projector elsewhere
    };
    ProjMode projMode      = ProjMode::Auto;
    float    triplanarScale = 0.25f;   // tiles per meter, only used by Triplanar
    bool     flipY          = false;   // toggle if the publisher sends upside-down

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
