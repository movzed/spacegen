#pragma once
// UvAtlasGeogram — alternative UV atlas backend using Bruno Lévy's
// geogram library (Spectral LSCM + Tetris packing). API mirrors
// UvAtlas::generateAtlas so callers can switch engines at runtime.
//
// Compiled into the binary only when -DSPACEGEN_ENABLE_GEOGRAM=ON is
// passed to cmake. When disabled, `generateAtlasGeogram()` returns a
// result with `ok = false` and a descriptive error message so the panel
// can show "Geogram not compiled in this build".
//
// Why optional?
//   geogram is ~50K LOC; pulling and building it adds 3-5 min to a
//   clean build. xatlas (the default) covers 95% of the use case for
//   SpaceGen, and Tier 1-3 already get RizomUV-class quality on top
//   of xatlas. Geogram is the nuclear option for meshes where xatlas's
//   chart segmentation is the problem (see next_tasks/tier_4_geogram_swap
//   README for decision criteria).

#include "UvAtlas.h"     // reuse UvAtlasResult / UvAtlasOptions / ProgressFn

namespace spacegen {

// Same shape as generateAtlas(). When SPACEGEN_HAVE_GEOGRAM is not
// defined, returns res.ok=false / res.error explaining the build was
// not configured with geogram.
UvAtlasResult generateAtlasGeogram(const MeshData& mesh,
                                    const UvAtlasOptions& opts = {},
                                    ProgressFn progressCb = nullptr,
                                    void* progressUser = nullptr);

// True when the engine was built with SPACEGEN_ENABLE_GEOGRAM=ON.
bool geogramAvailable();

} // namespace spacegen
