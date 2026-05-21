#pragma once
// BeamLayer — a moving-head-style spot light. Cone of light emitted from
// `origin`, pointed via pan/tilt (yaw/pitch) like a real intelligent fixture.
// LFOs on pan, tilt and intensity provide the classic stage-light movement
// effects (sweeps, figure-8s, breathing intensity, strobing).
//
// In projection-mapping mode (followCamera=true) the origin tracks the
// scene camera each frame, and pan/tilt rotate the beam relative to the
// camera's forward direction.

#include "Layer.h"
#include "LFO.h"

namespace spacegen {

class BeamLayer : public ILayer {
public:
    BeamLayer();

    LayerKind   kind()     const override { return LayerKind::Generator; }
    const char* typeName() const override { return "Spot Light"; }
    void        render(RenderContext& /*ctx*/) override {}  // consumed by StructureLayer
    void        drawInspector() override;

    // ---- Position ----
    glm::vec3 origin       = glm::vec3(0.0f, -6.0f, 1.0f);
    bool      followCamera = true;  // if true, origin = ctx.cameraWorldPos each frame

    // ---- Aim (moving head) ----
    // Pan / tilt are degrees relative to the base forward axis.
    // - When followCamera = true, base forward = ctx.cameraForward.
    // - When followCamera = false, base forward = world +Y.
    // pan=0, tilt=0 -> straight ahead. tilt positive = up, negative = down.
    float     panDeg       = 0.0f;
    float     tiltDeg      = 0.0f;

    // ---- Light shape ----
    glm::vec3 color        = glm::vec3(1.0f, 1.0f, 1.0f);
    float     intensity    = 5.0f;
    float     range        = 100.0f;
    float     innerDeg     = 5.0f;   // full-intensity half-angle
    float     outerDeg     = 9.0f;   // edge half-angle (soft falloff)

    // ---- LFO modulators (moving head choreography) ----
    // Per-axis LFOs (offsets added independently to base pan/tilt/intensity)
    LFO       panLFO;
    LFO       tiltLFO;
    LFO       intensityLFO;

    // Multi-axis motion pattern (Circle / Figure8 / Ballyhoo / Wave / etc.).
    // Adds offsets on top of the per-axis LFOs above. Off by default.
    MotionLFO motionLFO;

    // Compute the world-space pointing direction at time t given a base
    // forward axis (camera forward or world +Y).
    glm::vec3 directionAtTime(double t, const glm::vec3& baseForward) const;
    float     intensityAtTime(double t) const;
};

} // namespace spacegen
