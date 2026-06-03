#pragma once
// SDFParticleLayer — GPU-resident particle system bound to the scene SDF.
//
// Up to 1 M particles are stored in a Metal buffer of 32-byte `Particle`
// structs. Each frame:
//   1. A compute pass advects them through a curl-noise velocity field
//      (Bridson 2007) and applies a force from the gradient of an
//      implicit SDF derived from the scene bounding box.
//   2. A render pass draws them either as additive point sprites
//      (trailLength == 1) or as line-strip trails (trailLength > 1)
//      against the structure's depth buffer.
//
// All GPU work is encoded into `ctx.cmdBuf` directly; the layer owns its
// pipeline state, buffers, and shader library — `MetalRenderer` only needs
// to expose the depth texture and the camera matrices (already public via
// `ctx`). See INTEGRATION.md for plumbing.
//
// See README.md for algorithm spec; Bridson 2007 for curl noise; Inigo
// Quilez SDF canon for distance + gradient.

#include "../../../core/Layer.h"

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

namespace MTL {
    class Buffer;
    class ComputePipelineState;
    class RenderPipelineState;
    class DepthStencilState;
    class Library;
    class Texture;
}

namespace spacegen {

class SDFParticleLayer : public ILayer {
public:
    SDFParticleLayer();
    ~SDFParticleLayer() override;

    SDFParticleLayer(const SDFParticleLayer&)            = delete;
    SDFParticleLayer& operator=(const SDFParticleLayer&) = delete;

    // ---- ILayer interface ----
    LayerKind   kind()      const override { return LayerKind::Generator; }
    const char* typeName()  const override { return "SDF Particles"; }
    void        render(RenderContext& ctx) override;
    void        drawInspector() override;

    // ---- Public parameters (operator-controlled) ----

    enum class EmitterType : int {
        Surface = 0,   // Random point on the structure mesh
        Volume  = 1,   // Random point in the scene bbox
        Point   = 2,   // Fixed origin (pointOrigin)
    };

    // Particle population. Realloc on change (rare; expensive).
    // Clamped to [1024, 1'000'000]; further clamped to 256k when
    // trailLength > 1 to keep trail-ring memory under control.
    int   particleCount = 100000;

    // Per-particle life in seconds (± 30 % jitter applied at spawn).
    float lifetime      = 8.0f;

    // Curl-noise field parameters.
    // curlScale ≈ 1/wavelength (spatial frequency).
    // curlAmplitude in m/s — the velocity contribution at peak noise.
    // Defaults tuned LOW + short-wavelength so Surface-emitted particles
    // stay clinging to the structure (gentle shimmer) rather than being
    // swept off it. NOTE: the SDF force/crawl/collision currently reference
    // the scene BOUNDING BOX, not the mesh, so they pull particles toward
    // the box shell — they're left OFF by default until a real mesh SDF
    // bake lands (tracked separately). With no SDF force + tiny curl,
    // particles simply drift near their birth point on the mesh surface.
    float curlScale     = 1.2f;
    float curlAmplitude = 0.15f;

    // SDF force.
    //  > 0: attract  (particles pulled toward the structure surface)
    //  < 0: repel    (particles pushed away)
    // |sdfStrength| up to ~5; bigger = harder pull.
    // Default 0: the SDF is the bbox (wrong surface), so any pull drags
    // particles off the mesh toward the box. Re-enable once a mesh SDF
    // exists.
    float sdfStrength   = 0.0f;

    // SDF influence falloff (meters). Force ∝ exp(-(d/sdfRange)²).
    float sdfRange      = 0.5f;

    // Velocity damping (per second). 0 = no drag, 1 = halve every second.
    // Higher default so particles bleed speed and hover near their birth
    // point on the mesh instead of drifting off (cheap surface-cling).
    float drag          = 0.40f;

    // Per-particle color over a 3-stop life ramp:
    //   t<0.5 : mix(colorStart, colorMid, t*2)
    //   t>=0.5: mix(colorMid,   colorEnd, (t-0.5)*2)
    // RGB may exceed 1.0 (HDR) so the downstream Bloom catches the energy.
    // Alpha is multiplied by the alpha-curve sample at age/life.
    glm::vec4 colorStart = glm::vec4(2.4f, 1.3f, 0.4f, 1.0f); // hot HDR amber
    glm::vec4 colorMid   = glm::vec4(1.1f, 0.5f, 1.4f, 1.0f); // magenta core
    glm::vec4 colorEnd   = glm::vec4(0.15f, 0.35f, 1.2f, 0.0f); // cool / fade

    // Sampled alpha curve (4 points = ease-out by default). Linear-interp
    // in shader. Domain: x = age/life ∈ [0,1]. Range: y = alpha multiplier.
    glm::vec4 alphaCurve = glm::vec4(1.0f, 0.95f, 0.55f, 0.0f);

    // Sampled size-over-life curve (4 points, multiplies pointSizePx /
    // trail width). Domain t = age/life. Default = quick bloom then taper.
    glm::vec4 sizeCurve  = glm::vec4(0.4f, 1.0f, 0.8f, 0.15f);

    // Trail length: 1 = point sprites, 2..32 = ribbon-line trails.
    int   trailLength    = 1;

    // Point sprite size in pixels (only if trailLength == 1).
    float pointSizePx    = 4.0f;

    // Emitter choice.
    EmitterType emitter      = EmitterType::Surface;
    glm::vec3   pointOrigin  = glm::vec3(0.0f, 0.0f, 0.0f); // for Point mode

    // ---- Affectors (Notch-style) ----------------------------------------
    // Projection-mapping rule: particles must read as crawling ON or within
    // ~5 cm of the physical structure surface, never a free cloud drifting
    // into empty air. Surface-crawl is ON by default; keep curl short-
    // wavelength (high scale, modest amplitude); bias the SDF force to
    // attract so escapees snap back.

    // Surface-crawl: project the curl-noise velocity onto the SDF tangent
    // plane (remove the surface-normal component). DEFAULT OFF until the
    // SDF references the real mesh — right now it uses the bounding box, so
    // "crawling along the surface" means crawling along the BOX, which
    // pulls particles off the structure.
    bool  surfaceCrawl   = false;

    // SDF collision "hug": keep particles within skinOffset of the surface.
    // DEFAULT OFF for the same bbox-vs-mesh reason.
    bool  collisionEnable = false;
    // Skin band thickness (m). Particles closer than this are pushed out to
    // exactly skinOffset; ~5 cm reads as "painting" the structure.
    float skinOffset     = 0.05f;
    // Bounce on inward velocity at the skin. 0 = stick, 1 = perfect bounce.
    float restitution    = 0.10f;
    // Tangential drag applied while hugging (per second) — bleeds speed so
    // particles settle and crawl rather than skating off.
    float tangentDrag    = 2.0f;

    // Vortex affector: swirl around an axis through a center, within radius.
    bool      vortexEnable   = false;
    glm::vec3 vortexAxis     = glm::vec3(0.0f, 1.0f, 0.0f); // normalized at pack
    glm::vec3 vortexCenter   = glm::vec3(0.0f, 0.0f, 0.0f); // 0 => bbox center
    bool      vortexUseBBoxCenter = true;
    float     vortexStrength = 2.0f;                        // rad/s scale
    float     vortexRadius   = 2.0f;                        // m falloff

    // ---- ModulatorBank bindings (LFO/audio-reactive plumbing) ----
    int   curlAmpModSlot      = 0;
    float curlAmpModDepth     = 1.0f;
    int   sdfStrengthModSlot  = 0;
    float sdfStrengthModDepth = 1.0f;
    int   vortexStrengthModSlot  = 0;
    float vortexStrengthModDepth = 1.0f;

private:
    // -------- GPU resources --------

    // The full particle population (struct Particle below, 32 B each).
    MTL::Buffer* particleBuf_ = nullptr;
    size_t       allocatedCount_ = 0;

    // Trail ring (only allocated when trailLength > 1):
    // layout: [particle_i].pos[(head + 1) % K] is the freshly-written sample;
    // ring entries are 16 B each (float3 + 4 B pad — matches MSL alignment).
    MTL::Buffer* trailRingBuf_ = nullptr;
    // Per-particle head index (uint32). Separate buffer to keep ring stride
    // clean. Sized to match particleBuf_ capacity.
    MTL::Buffer* trailHeadBuf_ = nullptr;
    int          allocatedTrailLength_ = 0;

    // Mesh emission CDF and tri-verts (for Surface emitter).
    MTL::Buffer* triCDFBuf_   = nullptr;   // float prefix sum, [N_tris]
    MTL::Buffer* triVertsBuf_ = nullptr;   // float3 × 3 × N_tris (flattened)
    uint32_t     triCount_    = 0;
    float        triTotalArea_ = 0.0f;
    glm::vec3    sceneBBoxMin_ = glm::vec3(0.0f);
    glm::vec3    sceneBBoxMax_ = glm::vec3(0.0f);
    bool         meshDataDirty_ = true;

    // Per-frame uniforms (small, set with setBytes — no buffer needed).
    // Mirrors `ParticleUniforms` in particles.metal.inc — keep in sync.
    struct Uniforms {
        glm::mat4 projection;
        glm::mat4 view;
        glm::vec4 cameraWorldPos;        // .xyz = pos, .w = aspect

        // .x = dt, .y = time, .z = drag, .w = particleCount
        glm::vec4 timing;
        // .x = curlScale, .y = curlAmplitude, .z = sdfStrength, .w = sdfRange
        glm::vec4 noiseSdf;
        // .xyz = bbox center, .w = lifetime base
        glm::vec4 bboxCenterLife;
        // .xyz = bbox half-extent, .w = pointSizePx
        glm::vec4 bboxExtentSize;
        // .xy = viewport size (px), .z = trailLength, .w = emitterType
        glm::vec4 viewportMode;
        // .rgba colorStart (3-stop ramp, t=0)
        glm::vec4 colorStart;
        // .rgba colorEnd (3-stop ramp, t=1)
        glm::vec4 colorEnd;
        // 4 alpha curve points (linear-interp domain 0..1)
        glm::vec4 alphaCurve;
        // .xyz pointOrigin, .w triCount
        glm::vec4 pointOriginTriCount;
        // .x triTotalArea, .y frameIndex, .z surfaceCrawl(0/1), .w collisionEnable(0/1)
        glm::vec4 misc;
        // .rgba colorMid (3-stop ramp, t=0.5)
        glm::vec4 colorMid;
        // 4 size-over-life curve points (linear-interp domain 0..1)
        glm::vec4 sizeCurve;
        // .x skinOffset, .y restitution, .z tangentDrag, .w vortexEnable(0/1)
        glm::vec4 collision;
        // .xyz vortex axis (unit), .w vortex strength (post-modulator)
        glm::vec4 vortexAxisStr;
        // .xyz vortex center, .w vortex radius
        glm::vec4 vortexCenterRadius;
    } u_;
    static_assert(sizeof(Uniforms) % 16 == 0, "Uniforms must be 16-byte aligned");

    // -------- Pipeline state (built on first render) --------
    MTL::Library*              library_         = nullptr;
    MTL::ComputePipelineState* updatePipeline_  = nullptr;
    MTL::ComputePipelineState* initPipeline_    = nullptr;
    MTL::RenderPipelineState*  spritePipeline_  = nullptr;
    MTL::RenderPipelineState*  trailPipeline_   = nullptr;
    MTL::DepthStencilState*    depthReadState_  = nullptr;
    bool                       pipelineReady_   = false;

    // Dummy 16-byte buffers bound at slots 4/5 when trail mode is off (K==1).
    // The update_particles kernel declares both buffers unconditionally;
    // Metal's argument-validation will fault on dispatch if anything at
    // those slots is unbound, even when the kernel never dereferences
    // them on the K==1 codepath. Keeping placebos around is simpler than
    // shipping a function-constant kernel variant.
    MTL::Buffer*               dummyTrailRing_  = nullptr;
    MTL::Buffer*               dummyTrailHead_  = nullptr;

    // Misc state.
    double   lastFrameTime_         = -1.0;
    int      framesSinceMeshUpload_ = 0;
    int      currentParticleCount_  = 0;
    int      currentTrailLength_    = 1;
    uint32_t spawnSeed_             = 0x9E3779B9u;
    bool     needsSeed_             = false;   // set by realloc, cleared after dispatch

    // -------- Internal helpers --------
    void buildPipelinesIfNeeded(MetalRenderer& renderer);
    void reallocBuffersIfNeeded(MetalRenderer& renderer, int newCount,
                                 int newTrailLength);
    void uploadMeshData(MetalRenderer& renderer, Scene& scene);
    void releaseAllGpuResources();
    void seedParticleBufferGPU();   // dispatch a 1-time init kernel
    void packUniforms(const RenderContext& ctx);
};

} // namespace spacegen
