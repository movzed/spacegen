#pragma once
// SpaceGen Layer system — minimal v1 of the layer-stack-architecture skill.
//
// Every visible element in SpaceGen is a `Layer`. Layers live in a `Bus`
// (the Master bus by default). The renderer walks the bus bottom-to-top
// and calls render(ctx) on each enabled layer.
//
// v1 limitations (intentional, expanded in later milestones):
//   - State is just Enabled / Disabled (no Bypassed / Mute / Solo yet)
//   - Blend modes limited to Add and Normal (more in compositing-blend-modes
//     skill commit)
//   - No Group layers / sub-stacks
//   - Single Master bus (no Preview / Aux yet)
//   - opacity / blendMode are plain fields, not yet `Parameter` objects
//     (parameter-graph-spacegen skill will wrap them later)

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/mat4x4.hpp>

namespace MTL { class CommandBuffer; class Texture; }
namespace spacegen { class MetalRenderer; struct Scene; }

namespace spacegen {

enum class LayerKind  { Generator, Effect };
enum class LayerState { Enabled, Disabled };
enum class BlendMode  { Normal, Add };  // Multiply / Screen / Overlay added later

// Per-frame context handed to each layer's render() call.
struct RenderContext {
    MetalRenderer*       renderer;         // for backend-specific helpers
    Scene*               scene;            // for cross-layer queries (lights, etc.)
    MTL::CommandBuffer*  cmdBuf;
    MTL::Texture*        colorTarget;      // accumulator: read+write for effects
    int                  width;
    int                  height;
    glm::mat4            projection;
    glm::mat4            view;
    glm::vec3            cameraWorldPos;
    glm::vec3            cameraForward;    // world-space camera forward
    double               elapsedSeconds;
    int                  frameIndex;
};

class ILayer {
public:
    virtual ~ILayer() = default;

    // Identity / kind.
    virtual LayerKind   kind()      const = 0;
    virtual const char* typeName()  const = 0;

    // Per-frame render. The layer is responsible for using
    // `ctx.renderer` to do backend work.
    virtual void render(RenderContext& ctx) = 0;

    // ImGui controls for the layer's params. Called by the Inspector when
    // this layer is selected.
    virtual void drawInspector() {}

    // ---- common state (public for simplicity in v1) ----
    LayerState  state     = LayerState::Enabled;
    BlendMode   blendMode = BlendMode::Add;
    float       opacity   = 1.0f;
    std::string name;
    uint32_t    id        = nextId();
    // Optional 4-char visual tag color (RGB 0..1) for the layer rack row.
    glm::vec3   colorTag  = glm::vec3(0.45f, 0.62f, 0.85f);

private:
    static uint32_t nextId() {
        static uint32_t counter = 0;
        return ++counter;
    }
};

// A Bus owns an ordered list of layers and renders them bottom-to-top.
class Bus {
public:
    std::string                              name = "Master";
    std::vector<std::unique_ptr<ILayer>>     layers;

    // Append a layer, returns a non-owning pointer.
    template<typename T, typename... Args>
    T* add(Args&&... args) {
        auto p = std::make_unique<T>(std::forward<Args>(args)...);
        T* raw = p.get();
        layers.push_back(std::move(p));
        return raw;
    }

    void remove(uint32_t layerId) {
        layers.erase(
            std::remove_if(layers.begin(), layers.end(),
                [&](const std::unique_ptr<ILayer>& l) {
                    return l && l->id == layerId;
                }),
            layers.end());
    }

    // Render all enabled layers in order.
    void render(RenderContext& ctx);
};

} // namespace spacegen
