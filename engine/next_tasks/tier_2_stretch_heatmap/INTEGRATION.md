# Tier 2 Integration Steps

Drop-in instructions for landing the stretch heatmap in ~30 minutes.

## 1. MSL fragment shader (MetalRenderer.cpp)

Open `engine/backends/metal/MetalRenderer.cpp`, find the `kStructurePbrMSL`
string and the fragment function `fs_main`. After the existing block that
computes `syphonSample`, BEFORE the line:

```msl
float3 baseMat   = u.baseColorRoughness.rgb
                 * u.matBaseColor.rgb
                 * baseTex.rgb;
```

Paste the contents of `StretchHeatmap.metal.inc`. The snippet overrides
`baseTex` with the heatmap when `u.modeFlags.y > 0.5`, then lets the
existing PBR pipeline run on top.

## 2. Uniform fields

The Uniforms struct already has `modeFlags` (vec4, .x = lightsOnly).
Document the new fields:

```cpp
// Was:  float4 modeFlags;  // .x emitLightsOnly (0/1), others reserved
// Now:  float4 modeFlags;  // .x emitLightsOnly, .y stretchHeatmap,
//                           // .z stretchMetric (0=ratio,1=symDir)
```

No struct size change.

## 3. Render-path plumbing

In `MetalRenderer::renderStructureMeshes()` find:

```cpp
u.modeFlags = glm::vec4(layer.emitLightsOnly ? 1.0f : 0.0f,
                         0.0f, 0.0f, 0.0f);
```

Replace with:

```cpp
u.modeFlags = glm::vec4(layer.emitLightsOnly       ? 1.0f : 0.0f,
                         heatmapEnabled            ? 1.0f : 0.0f,
                         heatmapMetric == 1        ? 1.0f : 0.0f,
                         0.0f);
```

Add two parameters to `renderStructureMeshes` (default false / 0 so old
call sites compile):

```cpp
void renderStructureMeshes(..., bool heatmapEnabled = false,
                                 int  heatmapMetric = 0);
```

## 4. StructureLayer wiring

In `engine/core/StructureLayer.cpp`, the layer collects state from the bus
and calls `renderStructureMeshes`. Forward the heatmap flags from a
panel-controlled source. Easiest: pass via global `gUvHeatmap{Enabled,Metric}`
set by the UV Analysis panel, read in StructureLayer just like the panel
already reads scene state.

Cleaner: add `bool stretchHeatmapEnabled; int stretchHeatmapMetric;` to
the `StructureLayer` itself, and have the UV Analysis panel mutate those
fields via a pointer the panel acquires through the bus walk.

## 5. Panel UI

In `engine/gui/Workstation.mm`, add the static flags near `gUvSharpPreCut`:

```cpp
static bool gUvShowStretchHeatmap = false;
static int  gUvStretchMetric      = 0;
```

Then paste `panel_toggle.snippet.mm` into `drawUvAnalysisPanel` at the
indicated insertion point.

## 6. Verify

Build, launch. Open UV Analysis panel. Toggle "Show stretch heatmap":
the structure should turn white where the parameterization is clean
(post-xatlas big-chart areas, the PRT_UVW mask) and red where it
stretches badly. Compare across Tier-1 settings to find the right
threshold for your mesh.

## 7. Optional cleanup

The heatmap currently still gets lit by directional/spot lights. For a
diagnostic-only flat view, the operator can disable all light layers
in the bus rack. We could add a "Flat (no lighting)" toggle alongside
the heatmap, but it's redundant with the existing layer enable/disable.
