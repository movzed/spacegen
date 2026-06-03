# MotionBlurEffect

A post-process motion blur for SpaceGen, implementing the classic reconstruction
filter described in McGuire 2012 [1] with the simplified velocity-driven gather
of Rosado 2007 [2]. Designed for a projection-mapping engine where the camera
is fixed at runtime, lights and effect layers animate, and the operator expects
believable smearing on fast pans, flicks, and beam sweeps without ghosting on
static geometry.

The effect is supplied in two paths:

* **Camera-only (Path A, default).** No velocity G-buffer. Per-frame camera
  motion is reconstructed from the difference of the current and previous
  projection × view matrices; the post-pass smears the color buffer along the
  resulting screen-space vector with N taps. Cheap, predictable, single-pass.
* **Per-object (Path B, optional).** A second render target (RG16F velocity)
  is written alongside color during the structure pass. The vertex shader
  outputs `currClipPos` and `prevClipPos` (the previous MVP times the current
  world position); the fragment shader writes the screen-space delta. Path A's
  post-process is replaced with a velocity-driven gather that reads the
  velocity texture per fragment.

The two paths share the same compositing shader (`motion_blur.metal.inc`,
toggled by a `useCameraOnly` shader-constant flag) and the same inspector. The
operator switches paths at runtime via the inspector checkbox; Path B requires
the renderer to have been built with `Velocity` attachments (see
`INTEGRATION.md`).

## Algorithmic spec

### Path A — camera-only

Given the current and previous view-projection matrices `VP_t`, `VP_{t-1}`,
for each output pixel `p`:

1. Reconstruct world position from NDC + linear depth. Because the structure
   pass does not export depth in v1, we sidestep this by assuming a *flat
   plane at unit depth* (NDC `z = 0`). This is exact for the camera's
   forward axis and degrades smoothly elsewhere — perfectly acceptable for
   the small inter-frame deltas typical in projection mapping.

   Concretely: a screen-space pixel at NDC `(x, y, 0, 1)` is projected back
   through `inverse(VP_t)` and then re-projected through `VP_{t-1}`. The
   difference of the two NDC-XY positions is the camera-only velocity for
   that pixel:

       prevNDC = VP_{t-1} · inverse(VP_t) · vec4(currNDC, 0, 1)
       velocity = (currNDC.xy - prevNDC.xy / prevNDC.w) × 0.5  // [-1,1] → [0,1]

2. Scale by `shutter` (1.0 = "real" 360° shutter, 0.5 = 180° shutter).
3. Gather `sampleCount` taps along `velocity`, centered at `p`, weighted by a
   smoothstep falloff (tap weights peak at the center, zero at the edges).
4. Mix the gather result with the original color by an `intensity` knob so
   the operator can dial the effect from off → full.

The reconstruction `inverse(VP_t)` is done once on the CPU per frame and
passed as a single 4×4 uniform — there is no per-pixel inverse.

### Path B — per-object velocity buffer

The structure pass renders to a second render target of format `RG16F` at
the same resolution as the color target. The shader is unchanged for color;
its vertex stage additionally outputs:

    currClipPos = projection × view × model × position
    prevClipPos = prevProjection × prevView × prevModel × position

and the fragment stage writes:

    velocity.rg = currClipPos.xy/currClipPos.w
                - prevClipPos.xy/prevClipPos.w
    velocity *= 0.5                                 // NDC → UV

The compositing pass reads `velocity` for the current pixel, scales by
`shutter`, and gathers `sampleCount` color taps along it. Because each layer
that writes to the velocity buffer contributes its own per-object motion
(animated lights, beam sweeps, deformed geometry — anything that animates
the MVP), the smear correctly tracks per-object motion, not just camera
rotation.

The "previous" matrices are owned by `MetalRenderer` and shifted in
`renderFrame()` *before* the structure pass runs (so the just-rendered MVPs
become the previous-frame MVPs for the next frame's velocity computation).

### Sample distribution and depth weighting

The shared shader follows McGuire 2012 §2 ("plausible reconstruction"):

* Samples are placed at `i / (N-1)` along the velocity vector, jittered by
  a per-pixel hash so banding from the discrete tap count doesn't quantize
  on slow motion.
* Tap weight is `smoothstep(0, 0.5, 0.5 - |i/(N-1) - 0.5|)` — peaked at the
  center, smoothly fading at the trail's ends. This avoids the "step ladder"
  artifact of equal-weight box blurs that plagues Rosado 2007's original
  formulation.
* In Path B, an optional "closer wins" depth weight can be enabled (skipped
  in v1; the structure pass does not export linear depth as a sampleable
  resource yet — see `INTEGRATION.md` for the upgrade path).

### Shutter, sample count, intensity

| Knob          | Range      | Meaning                                       |
|---------------|------------|-----------------------------------------------|
| `sampleCount` | 4 … 32     | Number of taps along velocity. 4 = chunky;    |
|               |            | 16 = sweet spot; 32 = silky on cinema cuts.   |
| `shutter`     | 0 … 2      | Multiplier on velocity. 1 = physically        |
|               |            | plausible at 60 fps; 2 = exaggerated.         |
| `intensity`   | 0 … 1      | Blend factor between original and smeared.    |
| `useCameraOnly` | bool     | A vs B. Default true.                         |

### Performance

* Path A: one full-screen pass, `sampleCount` taps per pixel. At 1920×1080
  with `sampleCount=16` this is ~33 M texture fetches per frame ≈ 0.3 ms on
  M1.
* Path B: additionally writes an RG16F target during the structure pass
  (negligible — bandwidth-bound on the color, not the velocity). The post
  pass is identical to A but the per-pixel velocity read costs one extra
  texture fetch. Same ~0.3 ms post pass; structure pass cost rises ~10%
  from the second attachment write.

### Limitations (v1)

* No depth weighting (would need linear depth read). Acceptable because
  projection mapping has very shallow depth complexity at the scale where
  motion blur matters (light and beam layers, not heavy occluded geometry).
* No NeighborMax 8×8 tile pass (McGuire 2012's section 2.2 optimization).
  v1 gathers directly along the per-pixel velocity. Tile-based optimization
  is a v2 task — left as a TODO in the shader.
* No translucent / alpha-aware reconstruction. Premultiplied blending is
  honored but a layer that draws semi-transparently into the color buffer
  before the effect runs will smear its own alpha (correct in the common
  case, wrong if you wanted to read the structure underneath).

### Layer-system contract

* `kind() == LayerKind::Effect`. The layer reads `ctx.colorTarget`, writes
  back into the same texture via a transient internal scratch (ping-pong).
* `state == Enabled` runs the pass. `Disabled` is a no-op.
* `opacity` is folded into `intensity` — `effectiveIntensity = intensity ×
  opacity`. Disabling via the layer rack therefore fades it out cleanly.
* `blendMode` is fixed to `Normal` — motion blur is not additive.

## References

1. McGuire, M., Hennessy, P., Bukowski, M., Osman, B. **A Reconstruction
   Filter for Plausible Motion Blur.** I3D 2012.
   <https://casual-effects.com/research/McGuire2012Blur/>
2. Rosado, G. **GPU Gems 3, Chapter 27: Motion Blur as a Post-Processing
   Effect.** Addison-Wesley, 2007.
3. Sousa, T. **Stable SSAO in Battlefield 3 with selective temporal
   filtering.** GDC 2011. (Anchor for the smoothstep tap weighting.)
4. Jimenez, J. **Practical Real-time Strategies for Accurate Indirect
   Occlusion.** SIGGRAPH 2016. (Hash-based jitter, used here too.)
