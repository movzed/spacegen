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

#include <vector>

namespace spacegen {

class BeamLayer : public ILayer {
public:
    BeamLayer();

    LayerKind   kind()     const override { return LayerKind::Generator; }
    const char* typeName() const override { return "Spot Light"; }
    void        render(RenderContext& /*ctx*/) override {}  // consumed by StructureLayer
    void        drawInspector() override;

    // ---- Rig layout ----
    // The layer is conceptually a rig of N identical fixtures arranged
    // symmetrically around the center (camera if followCamera, else origin).
    // All fixtures share color / intensity / pan / tilt / cone. Only their
    // world position differs.
    enum class Layout : int {
        Single = 0,   // 1 fixture at the center
        Linear = 1,   // N fixtures evenly spaced on the camera's right axis
        Arc    = 2,   // N fixtures on a horizontal arc, all looking inward
    };

    Layout   layout       = Layout::Single;
    int      fixtureCount = 1;       // 1..8 (max 4 per side around center)
    float    spacing      = 1.0f;    // meters between adjacent fixtures (Linear)
    float    arcRadius    = 4.0f;    // distance from center for Arc layout
    float    arcSpreadDeg = 90.0f;   // total angular spread for Arc layout

    // Per-fixture phase distribution for ALL LFOs (per-axis + motion).
    // 0.0 -> every fixture sees the LFOs in sync (vertical sweep up/down).
    // 1.0 -> the LFO cycle is distributed evenly across fixtures, producing
    //        chase effects (figure-8s "pursue each other", waves travel).
    // Intermediate values blend continuously.
    float    fixturePhase = 0.0f;    // 0..1

    // Per-fixture colors. If `useFixtureColors == true`, fixture i uses
    // fixtureColors[i] (mod size). Otherwise all fixtures use `color`.
    bool                    useFixtureColors = false;
    std::vector<glm::vec3>  fixtureColors;   // resized lazily to fixtureCount

    // Returns world-space positions of the N fixtures.
    std::vector<glm::vec3> fixturePositions(const RenderContext& ctx) const;

    // Per-fixture helpers (account for fixturePhase and useFixtureColors).
    // `mods` is the global ModulatorBank from Scene; pass nullptr to skip
    // modulator contributions (useful for unit tests / preview).
    glm::vec3 colorForFixture(int idx) const;
    glm::vec3 directionAtTimeForFixture(double t, int idx, int total,
                                          const glm::vec3& baseForward,
                                          const class ModulatorBank* mods = nullptr) const;
    float     intensityAtTimeForFixture(double t, int idx, int total,
                                          const class ModulatorBank* mods = nullptr) const;

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

    // ---- Global modulator-bank bindings ----
    // Each field can subscribe to one of N global ModulatorBank slots.
    // slot 0 = unbound. Slot's output (normalized -1..+1) is multiplied
    // by the depth value and added to the base parameter.
    int   panModSlot       = 0;
    float panModDepth      = 30.0f;
    int   tiltModSlot      = 0;
    float tiltModDepth     = 20.0f;
    int   intensityModSlot = 0;
    float intensityModDepth = 5.0f;

    // Compute the world-space pointing direction at time t given a base
    // forward axis (camera forward or world +Y).
    glm::vec3 directionAtTime(double t, const glm::vec3& baseForward) const;
    float     intensityAtTime(double t) const;
};

} // namespace spacegen
