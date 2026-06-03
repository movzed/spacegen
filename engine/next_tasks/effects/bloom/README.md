# BloomEffect — Karis / Jimenez bloom (SIGGRAPH 2014)

`BloomEffect` is a `LayerKind::Effect` post-process that runs over the offscreen
color target after the Generator layers have written into it. The composition
window in `Workstation.mm` samples that same offscreen texture into an
`ImGui::Image`, so the effect "just appears" in the central viewport — no
changes to the dockspace, the workstation, or the composition window are
needed.

The algorithm is the one popularised by Jorge Jimenez in his SIGGRAPH 2014
talk *Next Generation Post Processing in Call of Duty: Advanced Warfare*
(Activision / Sledgehammer), combined with the bright-pass / soft-knee
threshold from Brian Karis's *Real Shading in Unreal Engine 4* course notes
(SIGGRAPH 2013 / Epic Games). It is the canonical "modern AAA bloom" and is
used (with tweaks) in UE4/5, Unity HDRP, Frostbite, Decima, etc.

## Why this technique

A naïve gaussian bloom on a HDR scene exhibits two killer artifacts:

1. **Pixel-art "boxiness"** at high intensities — a single bright pixel
   produces a hard square halo because the gaussian kernel is sampled at the
   final mip level.
2. **Firefly aliasing** — a single subpixel-bright sample blows up into a
   stable bright halo. This is fatal for projection mapping because the
   resulting bloom flickers wherever the bright pass jitters across pixel
   boundaries.

Jimenez's contribution is the **partial-Karis-average 13-tap downsample**
which (a) acts as a 5×5-tap blur in disguise (so the dual-filtering pyramid
has a wide effective kernel even at low mip levels), and (b) applies a Karis
average over four sub-quads of the 13-tap pattern. The Karis average
suppresses fireflies by computing the weighted mean of four samples weighted
by `1 / (1 + luminance)` — bright outliers contribute less. This is the same
trick Karis uses in his TAA filter (UE4 SIGGRAPH 2014).

The **9-tap tent upsample** is a separable-3-by-3 tent filter (Hammon's
"super-sample upsample"). The tent's triangular impulse response gives the
classic soft, glowy look without the wide ringing of a box filter or the
expensive evaluation of a true gaussian.

## Pipeline

```
                                                colorTarget (ctx.colorTarget)
                                                       │
                                              ┌────────┴────────┐
                                              │  threshold pass  │   ← bright-pass
                                              └────────┬────────┘
                                                       ▼
                                              ┌────────────────┐
                                              │   mip[0]  ½    │
                                              └────────┬───────┘
                                                       │ downsample13
                                                       ▼
                                              ┌────────────────┐
                                              │   mip[1]  ¼    │
                                              └────────┬───────┘
                                                       │   …
                                                       ▼
                                              ┌────────────────┐
                                              │   mip[N-1] 1/2ᴺ│
                                              └────────┬───────┘
                                                       │ upsample9 (additive)
                                                       ▼
                                              ┌────────────────┐
                                              │   mip[N-2]     │
                                              └────────┬───────┘
                                                       │   …
                                                       ▼
                                              ┌────────────────┐
                                              │   mip[0]       │   ← full bloom pyramid
                                              └────────┬───────┘
                                                       │ composite (reads colorTarget + mip[0])
                                                       ▼
                                              ┌────────────────┐
                                              │ compositeOut   │
                                              └────────┬───────┘
                                                       │ blit
                                                       ▼
                                                colorTarget
```

`N = 4..7` controlled by the `radius` knob in the inspector — bigger radius =
deeper mip chain = wider halo and slower (but only by a few %, because each
extra mip is ¼ the work of the previous one).

Total cost on a 1920×1080 colorTarget with N=6 mips, A14:
- ~0.4 ms threshold + 5×downsample + 6×upsample + composite

(Numbers are illustrative; real perf depends on the device and resolution.)

## 13-tap downsample kernel (Jimenez SIGGRAPH 2014, slide 153)

The 13-tap pattern samples the source at one center tap plus 12 surrounding
points. The kernel is split into five 2×2 sub-quads (one center + 4 corners),
each averaged on its own; the five sub-quad averages are then summed with
fixed weights:

```
sample layout (offsets in source-texel units):

      C─D       D
       │       │
   ┌───O───────O───┐
   │   │ A │ B │   │       O = mip-N source samples (the input texture)
   │   │   │   │   │       A,B,C,D = inner sub-quad centers (interpolated taps)
   │ A─O───┼───O─B │       E = exact center sample
   │   │   │   │   │
   │   │   E   │   │       (See Jimenez 2014 slide 153 for the figure.)
   │   │   │   │   │
   │ C─O───┼───O─D │
   │   │   │   │   │
   │   │   │   │   │
   └───O───────O───┘
       │       │
      A─B       B
```

In code (MSL), with `texelSize = 1.0 / sourceSize` and `uv` = the dst pixel's
center mapped to source UV:

```
// 13 taps, 6 "corners" + 6 "edges" + 1 center
float3 a = src.sample(s, uv + texelSize * float2(-2,  2)).rgb;
float3 b = src.sample(s, uv + texelSize * float2( 0,  2)).rgb;
float3 c = src.sample(s, uv + texelSize * float2( 2,  2)).rgb;
float3 d = src.sample(s, uv + texelSize * float2(-2,  0)).rgb;
float3 e = src.sample(s, uv + texelSize * float2( 0,  0)).rgb;
float3 f = src.sample(s, uv + texelSize * float2( 2,  0)).rgb;
float3 g = src.sample(s, uv + texelSize * float2(-2, -2)).rgb;
float3 h = src.sample(s, uv + texelSize * float2( 0, -2)).rgb;
float3 i = src.sample(s, uv + texelSize * float2( 2, -2)).rgb;
float3 j = src.sample(s, uv + texelSize * float2(-1,  1)).rgb;
float3 k = src.sample(s, uv + texelSize * float2( 1,  1)).rgb;
float3 l = src.sample(s, uv + texelSize * float2(-1, -1)).rgb;
float3 m = src.sample(s, uv + texelSize * float2( 1, -1)).rgb;

// 5 sub-quads: 4 corners @ weight 0.125 each, 1 center @ weight 0.5
float3 q0 = (j + k + l + m) * 0.25;   // inner 2x2
float3 q1 = (a + b + d + e) * 0.25;   // top-left
float3 q2 = (b + c + e + f) * 0.25;   // top-right
float3 q3 = (d + e + g + h) * 0.25;   // bot-left
float3 q4 = (e + f + h + i) * 0.25;   // bot-right

return q0 * 0.5 + (q1 + q2 + q3 + q4) * 0.125;
```

On the **first downsample only**, each sub-quad's contribution is divided by
`1 + lumaKaris(sub-quad)` (the Karis weighting) to crush fireflies before
they propagate into the chain. Higher mips skip this — by then everything
has been averaged enough that lone fireflies are already gone.

## 9-tap tent upsample (Jimenez 2014, slide 162)

A 3×3 tent filter applied to the lower mip, additively blended onto the
current mip:

```
// uv is the destination pixel mapped to the lower-mip's UV; texelSize is
// the lower mip's texel size, scaled by `radius` (Jimenez's "scatter"):
float3 a = src.sample(s, uv + texelSize * float2(-1,  1)).rgb;
float3 b = src.sample(s, uv + texelSize * float2( 0,  1)).rgb * 2.0;
float3 c = src.sample(s, uv + texelSize * float2( 1,  1)).rgb;
float3 d = src.sample(s, uv + texelSize * float2(-1,  0)).rgb * 2.0;
float3 e = src.sample(s, uv                                 ).rgb * 4.0;
float3 f = src.sample(s, uv + texelSize * float2( 1,  0)).rgb * 2.0;
float3 g = src.sample(s, uv + texelSize * float2(-1, -1)).rgb;
float3 h = src.sample(s, uv + texelSize * float2( 0, -1)).rgb * 2.0;
float3 i = src.sample(s, uv + texelSize * float2( 1, -1)).rgb;

return (a + b + c + d + e + f + g + h + i) * (1.0 / 16.0);
```

Per-mip output: `mip_dst = mip_dst + tent9(mip_src)` (additive blend on the
pipeline state, so the destination starts with whatever the downsample chain
wrote into it).

## Soft-knee threshold (Karis 2013, Unreal Engine 4 course notes)

The hard `max(0, luma - threshold)` causes a visible "pop" at the threshold
boundary — pixels just below it contribute nothing, pixels just above it
contribute fully. The soft-knee fix is a quadratic ramp around the threshold:

```
soft  = clamp(luma - (threshold - knee), 0, 2*knee);
soft  = soft * soft / (4*knee + 1e-4);
hard  = max(luma - threshold, 0);
boost = max(soft, hard) / max(luma, 1e-4);
return color * boost;
```

`knee` is the soft-knee parameter (0.5 by default = a generous fade-in
spanning ±50% of threshold). Setting `knee = 0` recovers the hard threshold.

## Composite

```
final = mix(scene, bloom * tint, intensity)
```

with `intensity` in `[0, 2]` (>1 over-boosts for the look of a saturated
sensor; useful with HDR-ish content). `tint` is a vec3 multiplied with the
bloom before mixing — set to `(1.0, 0.85, 0.55)` for the classic "gold rim"
look or `(0.6, 0.8, 1.0)` for an icy halation.

## References

- **Brian Karis** (Epic Games), *Real Shading in Unreal Engine 4*, SIGGRAPH
  2013 *Physically Based Shading in Theory and Practice* course.
  <https://blog.selfshadow.com/publications/s2013-shading-course/karis/s2013_pbs_epic_notes_v2.pdf>
  — origin of the Karis-average firefly filter and the soft-knee threshold.

- **Jorge Jimenez** (Activision / Sledgehammer Games), *Next Generation Post
  Processing in Call of Duty: Advanced Warfare*, SIGGRAPH 2014 *Advances in
  Real-Time Rendering* course.
  <https://www.iryoku.com/next-generation-post-processing-in-call-of-duty-advanced-warfare/>
  — origin of the 13-tap downsample + 9-tap tent upsample pyramid.

- **Earl Hammon Jr.** (Respawn), *PBR Diffuse Lighting for GGX+Smith
  Microsurfaces*, GDC 2017 — Hammon's notes also describe the 3×3 tent
  upsample variant used here.

- Bloom implementations cross-referenced for tap layout: UE4
  `Engine/Shaders/Private/PostProcessBloom.usf`, Unity HDRP
  `com.unity.render-pipelines.high-definition/Runtime/PostProcessing/Shaders/Bloom.compute`,
  Godot 4 `servers/rendering/renderer_rd/shaders/effects/bloom.glsl`.
