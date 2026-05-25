# Tier 3 Integration Steps

## 1. Add Eigen via FetchContent

In `engine/CMakeLists.txt`, after the xatlas block (~line 130):

```cmake
# ---- Eigen 3.4 (sparse linear algebra for SpaceGen Optimize) ----
# Header-only. ~50k LOC. MPL2 license (compatible with our use).
FetchContent_Declare(
    eigen
    GIT_REPOSITORY https://gitlab.com/libeigen/eigen.git
    GIT_TAG        3.4.0
    GIT_SHALLOW    TRUE
)
set(EIGEN_BUILD_DOC      OFF CACHE BOOL "" FORCE)
set(EIGEN_BUILD_TESTING  OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING        OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(eigen)
```

Then link in the `target_link_libraries(spacegen PRIVATE ...)` block:

```cmake
Eigen3::Eigen
```

Verify with `cmake --build .` — should pull eigen and not break anything
(Eigen is header-only, no .cpp).

## 2. Move source files into the engine

```bash
cp engine/next_tasks/tier_3_spacegen_optimize/UvOptimize.h   engine/core/
cp engine/next_tasks/tier_3_spacegen_optimize/UvOptimize.cpp engine/core/
```

Add to `add_executable(spacegen ...)` source list in CMakeLists.txt:

```cmake
    core/UvOptimize.cpp
```

## 3. Flesh out the SLIM solver

The skeleton in `UvOptimize.cpp` has the structure but two functions are
stubbed out:

- `extractCharts()` — union-find chart partition. ~80 LOC.
- `runSlimOnChart()` — SLIM iteration loop. ~200 LOC.
- `setupTriFrame()` (private helper) — per-triangle 3D→2D local frame
  with SVD precomputation. ~50 LOC.

Read `README.md` for the algorithm; follow SLIM paper §4 for the math.
The pseudocode comments inside `runSlimOnChart()` map each paper section
to an Eigen call.

## 4. Wire into the xatlas worker

In `engine/gui/Workstation.mm`, the worker thread already runs
`generateAtlas(...)` and saves the result. Add an OPTIONAL post-pass:

```cpp
// After the existing generateAtlas() call, before saving the cache:
if (opts.ok && gUvOptimizeRefine) {
    gUvState.stageId.store(6);  // new stage: "Optimising charts"
    spacegen::UvOptimizeOptions optOpts;
    optOpts.projectionAware = true;
    optOpts.viewDir         = -glm::vec3(meshPtr->transform[2]); // -Z forward
    spacegen::UvOptimizeResult refined = spacegen::optimiseUVs(
        *res, optOpts, uvWorkerOptimizeProgress, &gUvState);
    if (refined.ok) {
        res->uvs1 = std::move(refined.refinedUVs);
    } else {
        // log refined.error, but keep the unrefined result
    }
}
```

Add a stage 6 name to `uvWorkerProgress`:
```cpp
else if (std::strstr(stageName, "Optimising"))  stageId = 6;
```

Add the panel checkbox + slider next to the Sharp Edges section:

```cpp
ImGui::Checkbox("Refine with SpaceGen Optimize##refine", &gUvOptimizeRefine);
if (gUvOptimizeRefine) {
    ImGui::TextDisabled("Projection-aware SLIM stretch reducer.");
    ImGui::TextDisabled("Adds ~30s on top of xatlas; produces");
    ImGui::TextDisabled("RizomUV-class UV quality.");
}
```

## 5. View direction

Currently the panel doesn't know about the scene camera. Wire the
`viewDir` through `scene.camera.world[2]` (the camera-forward column of
the camera world matrix), captured at panel-button-click time.

```cpp
// In the lambda capture:
glm::vec3 viewDir = -glm::vec3(scene.camera.world[2]);
optOpts.viewDir = viewDir;
```

## 6. Test

Generate a fresh atlas with Tier 1 + Tier 3 enabled. Compare:

- Cache file size should be similar (same vertex count).
- xatlas log: same chart count.
- New log line: `[UvOptimize] N charts refined in T seconds, energy
  Σ_initial → Σ_final (-X%)`.
- Visually: the syphon video on flat regions should look LESS stretched,
  particularly on front-facing surfaces. Back-facing surfaces may show
  slightly worse stretch (deliberate: projection-aware weighting).

## 7. Performance budget

For the example scene (7.7M tri, ~500 charts post-Tier-1):

| Phase             | Time  | Notes                          |
|-------------------|-------|--------------------------------|
| extractCharts     |  ~5s  | Single-threaded union-find     |
| Per-chart SLIM    | ~25s  | Parallelised across cores      |
| Write-back        |  <1s  |                                |
| **Total Tier 3**  | ~30s  | On top of ~3min xatlas         |

Worth it: ~15% additional time for visibly better UVs.
