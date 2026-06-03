#pragma once
// AreaLightLayer — a finite-extent area light (rectangle or disc) that
// *orbits* the structure mesh along a parametric path. Contributes a
// representative-point PBR term to the structure pass alongside spots,
// directionals and ambients.
//
// HARD CONSTRAINT (enforced unconditionally by `update()`):
//   The light's reference position is projected every frame onto a sphere
//   of radius >= max(scene.bbox.maxDim) * safetyFactor centred on the
//   scene's geometric centroid. This guarantees the light never enters
//   the AABB of the mesh, so the operator never sees the "light vanished
//   inside the structure" flicker. The light *only orbits*.
//
// PBR model: Karis 2013, "Real Shading in Unreal Engine 4" §§ 4.1–4.3
//   — representative-point disc light with roughness expansion
//     α' = saturate(α + r / (2·dist)) for the GGX specular lobe, and the
//     closed-form Lambertian disc form-factor for the diffuse term.
//
// Consumed by `StructureLayer` (which collects all `AreaLightLayer*` from
// the bus) and forwarded to MetalRenderer::renderStructureMeshes(...).

#include "Layer.h"
#include "LFO.h"

namespace spacegen {

class AreaLightLayer : public ILayer {
public:
    AreaLightLayer();

    LayerKind   kind()     const override { return LayerKind::Generator; }
    const char* typeName() const override { return "Area Light"; }
    // Consumed by StructureLayer (like Beam / Directional / Ambient);
    // no direct render contribution.
    void        render(RenderContext& ctx) override;
    void        drawInspector() override;

    // ---- Shape ----------------------------------------------------------
    enum class Shape : int { Disc = 0, Rectangle = 1 };
    Shape     shape  = Shape::Disc;
    float     radius = 0.5f;      // disc radius (m)
    float     width  = 1.0f;      // rectangle width  (m)
    float     height = 0.5f;      // rectangle height (m)

    // Disc/rectangle facing. If `faceCentroid` is true (default), the
    // normal is recomputed each frame to point from the light position
    // back at the scene centroid (i.e. the panel always "looks at" the
    // structure). Otherwise the operator-supplied pan/tilt are used.
    bool      faceCentroid = true;
    float     panDeg       = 0.0f;    // ignored when faceCentroid == true
    float     tiltDeg      = 0.0f;

    // ---- Photometric ----------------------------------------------------
    glm::vec3 color     = glm::vec3(1.0f);
    float     intensity = 5.0f;
    float     range     = 60.0f;      // linear falloff, projection-mapping style

    // ---- Orbit ----------------------------------------------------------
    enum class OrbitMode : int {
        Circular   = 0,
        Lissajous  = 1,
        Helix      = 2,
        Random     = 3,
        // --- Articulated paths (richer choreography) ---------------------
        FigureEight = 4,   // Gerono lemniscate rolled onto the sphere
        Rose        = 5,   // rhodonea curve r=cos(k·θ), k-petal star
        Spiral      = 6,   // angle sweeps while polar radius breathes
        Pendulum    = 7,   // damped harmonic swing about the axis pole
        Compound    = 8,   // two stacked harmonics → quasi-periodic weave
    };
    enum class Axis : int { X = 0, Y = 1, Z = 2 };

    // Easing applied to the *path parameter* (the normalised phase that
    // drives a mode). Combined with `phaseOffset`, several lights with
    // staggered phase + easing read as one articulated, choreographed swarm.
    enum class Easing : int {
        Linear    = 0,   // u
        Smoothstep = 1,  // 3u²-2u³ (Hermite, C¹)
        EaseInOut = 2,   // 0.5-0.5·cos(π·u)  (sinusoidal)
        Bounce    = 3,   // decaying parabola bounce (Robert Penner)
    };

    OrbitMode orbitMode  = OrbitMode::Circular;
    Axis      orbitAxis  = Axis::Z;     // Circular + Helix + articulated

    // The *operator-requested* orbit radius. The actual world-space radius
    // is `max(orbitRadius, safeRadius)` to enforce the hard constraint.
    float     orbitRadius = 4.0f;

    // Safety factor: orbit sphere radius = max(scene AABB extents) * factor / 2,
    // clamped to a sensible minimum. 1.2 = 20% stand-off beyond the
    // circumscribed sphere of the AABB. See README §3.
    float     safetyFactor = 1.2f;

    // Circular + Helix: angular speed in degrees per second.
    float     speedDegPerSec = 18.0f;

    // Lissajous (read README §2.2).
    float     lissAx = 0.9f;     // amplitude on the first sphere-tangent axis
    float     lissAy = 0.6f;     // amplitude on the second
    float     lissA  = 1.0f;     // freq multiplier on x
    float     lissB  = 2.0f;     // freq multiplier on y (b/a forms the figure)
    float     lissPhi = 0.0f;    // phase offset on y, radians

    // Helix vertical bob (orbit-axis component).
    float     bobFreqHz = 0.25f;
    float     bobAmp    = 0.4f;  // 0..1 fraction of orbit radius

    // Random points: dwell between fresh samples (seconds); the layer
    // smoothly slerps between the previous and the next sample, eased
    // with `0.5 - 0.5*cos(π·u)` (smoothstep-like).
    float     dwellSeconds = 2.0f;
    float     slerpEase    = 1.0f;   // 0 = linear, 1 = full cosine ease

    // ---- Articulated path parameters -----------------------------------
    // FigureEight / Rose / Spiral / Pendulum / Compound all share the
    // operator-facing speed (deg/s) as their base angular rate, plus:
    float     pathAmplitude = 0.9f;  // tangent-plane reach (0..1.5), shared
    int       roseK         = 5;     // Rose: petal count (odd k → k petals,
                                      //        even k → 2k petals)
    float     spiralTurns   = 3.0f;  // Spiral: polar excursions per cycle
    float     spiralPolar   = 0.85f; // Spiral: max polar opening (0..1)
    float     pendDamping   = 0.6f;  // Pendulum: decay per cycle (0=undamped)
    float     pendSwingDeg  = 70.0f; // Pendulum: peak swing half-angle
    float     compoundRatio = 3.0f;  // Compound: 2nd harmonic freq multiplier
    float     compoundMix   = 0.45f; // Compound: blend of 2nd harmonic (0..1)

    // Per-light PHASE OFFSET (turns, 0..1) folded into the path parameter
    // before easing. Staggering this across several area lights produces a
    // choreographed, articulated swarm (lights chase each other along the
    // same path).
    float     phaseOffset = 0.0f;

    // Easing applied to the cyclic path parameter (see Easing enum). Wrapped
    // so the eased phase remains C⁰-continuous across the period seam.
    Easing    easing = Easing::Linear;

    // ---- Motion presets -------------------------------------------------
    // One-click choreography: sets orbitMode + speed + amplitude + easing
    // + phase together. `Custom` means "operator has hand-tuned values".
    enum class MotionPreset : int {
        Custom      = 0,
        SlowOrbit   = 1,
        Figure8Sweep = 2,
        RoseBloom   = 3,
        PendulumPair = 4,
        ChaosSwarm  = 5,
    };
    MotionPreset preset = MotionPreset::Custom;

    // Apply a preset's parameter bundle in-place. Leaves the safety clamp
    // and all photometric/shape fields untouched — purely motion authoring.
    void applyPreset(MotionPreset p);

    // ---- LFO modulators (optional intensity wobble) --------------------
    LFO       intensityLFO;

    // ---- Public read-only outputs (updated by update()) -----------------
    // These are filled in by `update()` (called once per frame from render()).
    // The renderer can grab them directly to pack into the uniform buffer.
    glm::vec3 positionWorld = glm::vec3(0.0f, 0.0f, 5.0f);
    glm::vec3 normalWorld   = glm::vec3(0.0f, 0.0f, -1.0f);   // disc/rect normal
    float     safeRadius    = 1.0f;     // computed from scene
    float     effectiveR    = 0.5f;     // representative radius (disc r, or rect equivalent)

    // Recomputes positionWorld + normalWorld from the orbit mode at time `t`,
    // then applies the safety clamp. Also caches `safeRadius` from the scene
    // bounds. Idempotent within a frame. Called from render().
    void update(const RenderContext& ctx);

    // Helper: compute the equivalent-area disc radius for the current shape.
    // (Used by the shader for both Disc and Rectangle paths.)
    float equivalentRadius() const;

private:
    // Internal state for the Random orbit mode.
    double     randomT0_      = 0.0;     // time of last sample
    glm::vec3  randomPrev_    = glm::vec3(0.0f, 0.0f, 1.0f);
    glm::vec3  randomNext_    = glm::vec3(0.0f, 1.0f, 0.0f);
    uint32_t   randomSeed_    = 0xC0FFEEu;
    bool       randomInited_  = false;

    glm::vec3 evalCircular_(double t)  const;
    glm::vec3 evalLissajous_(double t) const;
    glm::vec3 evalHelix_(double t)     const;
    glm::vec3 evalRandom_(double t);            // mutates randomPrev_/Next_

    // Articulated paths. Each returns a *unit-length* direction on the
    // sphere (same contract as the originals): update() scales by the
    // clamped radius, so the hard safety clamp still applies unchanged.
    glm::vec3 evalFigureEight_(double t) const;
    glm::vec3 evalRose_(double t)        const;
    glm::vec3 evalSpiral_(double t)      const;
    glm::vec3 evalPendulum_(double t)    const;
    glm::vec3 evalCompound_(double t)    const;

    // Maps elapsed time → an eased, phase-offset angle (radians) for the
    // cyclic modes. Folds in `phaseOffset` and the `easing` selector.
    float     pathAngle_(double t) const;
};

} // namespace spacegen
