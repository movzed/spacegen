#include "Layer.h"
#include <algorithm>

namespace spacegen {

void Bus::render(RenderContext& ctx) {
    for (auto& layer : layers) {
        if (!layer) continue;
        if (layer->state != LayerState::Enabled) continue;
        if (layer->opacity <= 0.0f) continue;
        layer->render(ctx);
    }
}

} // namespace spacegen
