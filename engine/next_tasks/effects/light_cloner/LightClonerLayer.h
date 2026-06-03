#pragma once
// LightClonerLayer — Generator layer that clones ONE light configuration
// into N virtual fixtures arranged via a distribution pattern (ring / helix
// / grid / Fibonacci-on-sphere / random). EMITS-MULTIPLE pattern: the
// renderer never sees clones — it just gets a larger spot/area light array.
//
// Inspired by Notch's Cloner node. See README.md in this folder for the
// algorithm spec, the Fibonacci golden-angle math, and the color theory
// references.
//
// Generic across light kinds: a `lightKind` field selects whether clones
// are expanded into the spot-light bus or the area-light bus. The area
// path is gated behind SPACEGEN_AREA_LIGHTS until AreaLightLayer ships.

#include "Layer.h"
#include "LFO.h"

#include <glm/glm.hpp>

#include <vector>

namespace spacegen {

class ModulatorBank;

// Output struct passed to the renderer. Mirror of MetalRenderer::GpuSpot
// — kept in this header so StructureLayer.cpp can build it without leaking
// the GpuSpot type out of the backend translation unit. The renderer packs
// the same four vec4s; see MetalRenderer.cpp.
struct VirtualSpot {
    glm::vec3 worldPos;
    glm::vec3 direction;     // FROM the light, normalized
    glm::vec3 color;
    float     intensity;
    float     range;
    float     innerCos;      // cos(innerDeg)
    float     outerCos;      // cos(outerDeg)
};

class LightClonerLayer : public ILayer {
public:
    LightClonerLayer();

    LayerKind   kind()     const override { return LayerKind::Generator; }
    const char* typeName() const override { return "Light Cloner"; }
    void        render(RenderContext& /*ctx*/) override {}  // consumed by StructureLayer
    void        drawInspector() override;

    // --------------------------------------------------------------
    // Light kind
    // --------------------------------------------------------------
    enum class LightKind : int {
        Spot = 0,
        Area = 1,        // virtual area lights; only useful once
                          // AreaLightLayer / area-light bus exists.
    };
    LightKind lightKind = LightKind::Spot;

    // --------------------------------------------------------------
    // Distribution
    // --------------------------------------------------------------
    enum class Pattern : int {
        Ring     = 0,    // equal-spaced ring around `origin`, in plane perp to `axis`
        Helix    = 1,    // ring + linear advance along `axis`
        Grid     = 2,    // gridCols × ceil(N/gridCols) in plane perp to `axis`
        FibSphere= 3,    // Fibonacci spiral on a sphere (golden angle)
        Random   = 4,    // deterministic pseudo-random points (seeded box)
    };
    Pattern pattern = Pattern::Ring;

    // Count of clones; 1..64. The renderer ceiling is enforced downstream
    // (kMaxSpots), but we cap here too so the inspector slider matches.
    int     clones        = 12;

    // Center of the distribution. If `followCamera`, the origin is set to
    // ctx.cameraWorldPos every frame; if `useSceneCentroid`, it tracks the
    // scene centroid (computed by StructureLayer); else this fixed value.
    glm::vec3 origin       = glm::vec3(0.0f, 0.0f, 2.0f);
    bool      followCamera = false;
    bool      useSceneCentroid = true;

    // The pattern's spine / "up" axis. Default: world +Z (vertical helix,
    // horizontal ring).
    glm::vec3 axis         = glm::vec3(0.0f, 0.0f, 1.0f);

    // Pattern-specific params.
    float     radius       = 3.0f;     // Ring / Helix / FibSphere base radius
    float     stepHeight   = 0.5f;     // Helix vertical step (meters per clone)
    float     turns        = 1.0f;     // Helix: total revolutions across N
    int       gridCols     = 4;        // Grid: columns; rows = ceil(N/gridCols)
    float     gridSpacing  = 1.0f;     // Grid: meters between adjacent cells
    float     randomRadius = 4.0f;     // Random: bounding box half-extent
    uint32_t  seed         = 0xC0DEFEEDu;

    // --------------------------------------------------------------
    // Per-clone phase distribution (animations stagger across the
    // index). See BeamLayer::fixturePhase for the same idea on 8
    // fixtures; here it scales to 64.
    // --------------------------------------------------------------
    float     phaseSpread  = 1.0f;   // 0 = synced, 1 = chase across all clones

    // --------------------------------------------------------------
    // Color scheme
    // --------------------------------------------------------------
    enum class ColorScheme : int {
        Uniform      = 0,
        HueChase     = 1,
        Gradient     = 2,
        Triadic      = 3,
        Complementary= 4,
    };
    ColorScheme colorScheme = ColorScheme::Uniform;
    glm::vec3   colorStart  = glm::vec3(1.0f, 1.0f, 1.0f);
    glm::vec3   colorEnd    = glm::vec3(0.1f, 0.4f, 1.0f);
    float       hueSpread   = 1.0f;   // HueChase: 1.0 = full wheel across N

    // --------------------------------------------------------------
    // Swarm motion (per-clone Lissajous offset on top of template aim).
    // --------------------------------------------------------------
    bool        swarmEnabled = false;
    float       swarmAmp     = 0.4f;     // meters of position drift
    float       swarmFreq    = 0.25f;    // Hz of the base axis
    float       swarmRatioY  = 1.31f;    // irrational ratio
    float       swarmRatioZ  = 0.79f;

    // --------------------------------------------------------------
    // Template light (the "master" config every clone inherits from)
    // --------------------------------------------------------------
    // We embed a self-contained subset of BeamLayer's controls inline,
    // so the cloner does not need to point at another layer.
    glm::vec3 templateColor      = glm::vec3(1.0f);   // used only when colorScheme=Uniform
    float     templateIntensity  = 4.0f;
    float     templateRange      = 60.0f;
    float     templateInnerDeg   = 5.0f;
    float     templateOuterDeg   = 9.0f;

    // Base aim, relative to the cluster's centroid → each clone's direction
    // points "outward" radially from the cluster by default; pan/tilt offset
    // that. When `aimInward=true`, the direction flips so all clones look
    // toward `origin` (great for shining a cluster onto a centerpiece).
    bool      aimInward          = false;
    float     panDeg             = 0.0f;
    float     tiltDeg            = 0.0f;

    // Per-axis LFOs and motion pattern (same vocabulary as BeamLayer).
    LFO       panLFO;
    LFO       tiltLFO;
    LFO       intensityLFO;
    MotionLFO motionLFO;

    // --------------------------------------------------------------
    // Expansion API — called by StructureLayer once per frame.
    // --------------------------------------------------------------
    // Appends N virtual spot lights to `sink`. Returns the number appended,
    // which is min(clones, available headroom). If the layer is disabled
    // or opacity is zero, appends nothing and returns 0.
    int expandSpots(const RenderContext& ctx,
                    std::vector<VirtualSpot>& sink) const;

    // Centroid the cloner is centered on this frame. Exposed for debug /
    // gizmo drawing; not currently used by the renderer.
    glm::vec3 effectiveOrigin(const RenderContext& ctx) const;

private:
    // Build positions of all clones in world space. The vector size
    // equals min(clones, 64).
    std::vector<glm::vec3> clonePositions(const glm::vec3& centroid) const;

    // Resolve color for clone i / N according to colorScheme.
    glm::vec3 cloneColor(int idx, int total) const;

    // Resolve direction for clone i: aim outward from centroid, with
    // panLFO/tiltLFO/motionLFO offsets evaluated at phase ph (= i/N *
    // phaseSpread). When aimInward, points back toward centroid.
    glm::vec3 cloneDirection(double t,
                              int idx, int total,
                              const glm::vec3& centroid,
                              const glm::vec3& clonePos,
                              const ModulatorBank* mods) const;

    // Per-clone Lissajous offset (zero unless swarmEnabled).
    glm::vec3 swarmOffset(double t, int idx, int total) const;
};

} // namespace spacegen
