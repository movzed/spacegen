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
    // Toggle if the publisher sends upside-down frames.
    bool      flipY = false;

    // The Syphon video is ALWAYS sampled by the mesh's UVs, with a single
    // per-fragment hybrid rule:
    //
    //   - Where the original Blender unwrap (UV0 / PRT_UVW) carries real
    //     data (the screen-space gradient of uv is nonzero on that face),
    //     the shader samples by UV0. This preserves the artist's intent on
    //     the unwrapped mask.
    //
    //   - Where UV0 is degenerate (all 3 verts of the triangle collapsed
    //     to the same UV point — no unwrap), the shader falls back to UV1
    //     (the xatlas-generated atlas). The atlas is tuned for FEW LARGE
    //     charts so the video flows continuously across the gap with
    //     minimal stretching.
    //
    // No mode toggle. The decision is automatic per-fragment.

    // ---- Projector-on-flat blend ----
    // When the per-fragment normal curvature (|dfdx(N)| + |dfdy(N)|) is
    // below `projectorFlatnessThreshold`, the surface is "locally flat"
    // in screen space and the atlas tends to allocate a small UV chart
    // → video looks zoomed. Mix in the projector NDC sample to recover
    // 1:1 projection-mapping correspondence on those big flat areas.
    //
    // Curved areas (3D details, circles, the mask) keep their full UV0
    // or UV1 atlas mapping — projector is OFF on them by construction.
    //
    //   projectorOnFlatMix = 0.0 → pure atlas (3D detail preserved
    //                              everywhere, flat walls may look zoomed)
    //   projectorOnFlatMix = 1.0 → pure projector NDC on flat walls,
    //                              atlas on curved (best of both worlds)
    float projectorOnFlatMix       = 0.0f;
    float projectorFlatnessThreshold = 0.05f;

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
