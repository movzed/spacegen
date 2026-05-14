# Engineering Brief — SpaceGen Next Shaders
Generated: 2026-05-14

> Written by the Research Coordinator after auditing the existing shader codebase
> (`ChaserShader`, `SaberShader`, `ParticleShader`, `GeometryShader`) and surveying
> the live-performance shader landscape (Synesthesia, ISF, Shadertoy, TouchDesigner,
> VJ Galaxy, Book of Shaders, Inigo Quilez's work, and academic papers through 2025).

---

## Architecture Notes (read first)

SpaceGen's current architecture:

- **Renderer**: Three.js `WebGLRenderer` with an orthographic camera; every shader
  renders a full-screen `PlaneGeometry(2,2)` (or `THREE.Points` for particles) with
  `AdditiveBlending` by default.
- **Layer contract**: Each shader class exposes `threeObject`, `uniforms`, `update(time, delta, beat, bpmClock)`, `buildGUI(folder)`, and optionally `onResize(w,h)`.
- **Mask system**: Any layer can receive a `sampler2D uMask` texture that gates output alpha; the `SaberShader` uses the mask as an *input image* (edge-detection on it), not just a gate.
- **BPM clock**: `beat` (0-1 within the current beat) and `bpmClock.bpm` are passed into every `update()`. No shader currently uses them — this is untapped potential.
- **No multi-pass / ping-pong yet**: The biggest missing capability. Adding a `RenderTargetPingPong` utility class would unlock ~8 of the shaders below.

---

## Priority Queue

### TIER 1 — Implement immediately (single-pass, no new infrastructure)

---

**1. PlasmaWave — Classic sine-sum color field**
Difficulty: Easy

- **Visual**: Smooth, undulating fields of color — classic "oldschool demo" plasma.
  Colors shift and ripple continuously across the whole frame.
- **Algorithm**: Three overlapping sine waves evaluated at each UV, summed, then
  mapped to HSV or a palette lookup. Add `uTime` to animate.
  ```glsl
  float v = sin(uv.x * uFreq1 + uTime);
  v += sin(uv.y * uFreq2 + uTime * 0.7);
  v += sin((uv.x + uv.y) * uFreq3 + uTime * 1.3);
  vec3 col = palette(v * 0.5 + 0.5, uColorA, uColorB, uColorC, uColorD);
  ```
  Use Inigo Quilez's cosine palette: `a + b*cos(2π*(c*t+d))`.
- **Parameters**: `uFreq1/2/3` (0.5–20), `uSpeed` (0–3), `uColorA/B/C/D` (four
  palette vec3s), `uContrast` (0–2), `uBrightness`.
- **BPM hook**: Pulse `uFreq1` on beat for a kick effect.
- **Reference**: https://www.bidouille.org/prog/plasma, https://iquilezles.org/articles/palettes/

---

**2. DomainWarp — Fractal Brownian Motion with recursive domain distortion**
Difficulty: Easy–Medium

- **Visual**: Flowing, organic smoke/lava-lamp shapes. Colors drift like living
  paint. One of the most versatile VJ effects in active use.
- **Algorithm**: Inigo Quilez's two-layer domain-warp:
  ```glsl
  vec2 q = vec2(fbm(uv + uTime*0.1), fbm(uv + vec2(5.2, 1.3)));
  vec2 r = vec2(fbm(uv + q + vec2(1.7,9.2) + 0.15*uTime),
                fbm(uv + q + vec2(8.3,2.8) + 0.126*uTime));
  float f = fbm(uv + r);
  ```
  4-octave value noise (`rand`+bilinear) is already in `ChaserShader` — reuse it.
- **Parameters**: `uWarpStrength` (0–2), `uScale` (0.5–8), `uSpeed` (0–1),
  `uColorLow/Mid/High` (three key colors), `uOctaves` (1–6, int).
- **BPM hook**: Spike `uWarpStrength` on beat.
- **Reference**: https://iquilezles.org/articles/warp/, https://www.shadertoy.com/view/wttXz8

---

**3. KaleidoscopeTile — UV folding mirror symmetry**
Difficulty: Easy

- **Visual**: Mandala-like geometric mirrors that repeat any underlying UV pattern
  (or the mask itself) into N-fold radial symmetry.
- **Algorithm**:
  ```glsl
  vec2 uv = vUv - 0.5;
  float angle = atan(uv.y, uv.x);
  float slice = 2.0 * PI / uSegments;
  angle = mod(angle, slice);
  if (angle > slice * 0.5) angle = slice - angle; // mirror
  uv = vec2(cos(angle), sin(angle)) * length(uv) + 0.5;
  // now sample any texture or noise at uv
  ```
  Pair with PlasmaWave or DomainWarp as the "source" UV to get infinite variety.
- **Parameters**: `uSegments` (2–24, int), `uRotSpeed` (0–2), `uZoom` (0.5–4),
  `uOffsetX/Y` (UV scroll), optional texture input.
- **Reference**: ISF "Kaleidoscope" — https://github.com/Vidvox/ISF-Files

---

**4. ChromaGlitch — Chromatic aberration + RGB scanline glitch**
Difficulty: Easy

- **Visual**: VHS/CRT glitch aesthetic. RGB channels split horizontally; random
  horizontal slices jump; scanline flicker. Works powerfully over the mask.
- **Algorithm**: Sample `uMask`/`uTexture` three times with per-channel UV offsets
  driven by hash-noise. Add occasional horizontal block displacement gated by a
  random threshold.
  ```glsl
  float glitch = step(uGlitchThreshold, rand(vec2(floor(uv.y * 30.0), uTime * uSpeed)));
  float shift = glitch * uShiftAmount * (rand(vec2(uTime, uv.y)) - 0.5);
  float r = texture2D(uSrc, uv + vec2(shift + uAberration, 0.0)).r;
  float g = texture2D(uSrc, uv + vec2(shift, 0.0)).g;
  float b = texture2D(uSrc, uv + vec2(shift - uAberration, 0.0)).b;
  ```
- **Parameters**: `uAberration` (0–0.05), `uGlitchThreshold` (0.8–1.0),
  `uShiftAmount` (0–0.1), `uSpeed` (0–5), `uScanlineIntensity` (0–1).
- **Note**: This is a **post-process** shader — it takes another layer's output as
  `uSrc`. Consider adding a "source layer" input mechanism to the GUI.

---

**5. StarField — Depth-scrolling layered stars**
Difficulty: Easy

- **Visual**: Classic infinite starfield fly-through with parallax layers and
  optional star-streak motion blur. Staple of space/electronic shows.
- **Algorithm**: Hash the UV cell to get star position and brightness. Three
  depth layers scrolling at different speeds along `uDirection`.
  ```glsl
  float stars(vec2 uv, float density, float size) {
    vec2 id = floor(uv * density);
    vec2 p  = fract(uv * density) - 0.5;
    vec2 jitter = vec2(rand(id), rand(id + 7.3)) - 0.5;
    float d = length(p - jitter * 0.8);
    return smoothstep(size, 0.0, d) * rand(id + 13.7);
  }
  ```
- **Parameters**: `uDensity` (20–200), `uSpeed` (0–3), `uDirection` (angle 0–360°),
  `uTwinkle` (0–1), `uColorCold/Warm` (star color range), `uStreak` (0–1 motion blur).
- **BPM hook**: Flash brightness on beat for strobe/pulse.

---

**6. VoronoiCells — Animated Worley/cellular noise**
Difficulty: Easy–Medium

- **Visual**: Organic cracked-glass or cell-division patterns. Color each cell by
  distance to its nearest seed point. Seeds animate over time.
- **Algorithm**: For each pixel, find the nearest of N random seed points (animated
  with `sin/cos` of time). Color = `1.0 - smoothstep(0, 0.05, minDist)` for borders,
  or `minDist` for cell fill.
  ```glsl
  vec2 cellUv = fract(uv * uScale + 0.5);
  vec2 cellId = floor(uv * uScale + 0.5);
  float minDist = 8.0;
  for (int x = -1; x <= 1; x++) for (int y = -1; y <= 1; y++) {
    vec2 neighbor = vec2(float(x), float(y));
    vec2 point = rand2(cellId + neighbor);
    point = 0.5 + 0.5 * sin(uTime * 0.3 + 6.28 * point);
    float d = length(cellUv - neighbor - point);
    minDist = min(minDist, d);
  }
  ```
- **Parameters**: `uScale` (2–20), `uSpeed` (0–2), `uBorderWidth` (0–0.1),
  `uColorA/B` (cell and border colors), `uFill` (0=border-only, 1=filled cells).
- **Reference**: https://thebookofshaders.com/12/

---

**7. OscilloscopeWave — Waveform + Lissajous display**
Difficulty: Medium

- **Visual**: A neon waveform line drawn as a glowing ribbon; optionally switches
  to XY (Lissajous) mode to draw beat-synced figures. Looks like an oscilloscope.
- **Algorithm**: For each pixel, compute distance to the curve
  `y = A*sin(uFreqX*x + uPhaseX)` (waveform mode) or
  parameterically `(sin(uFreqX*t), sin(uFreqY*t + uPhase))` (Lissajous mode).
  Apply distance-based glow with `exp(-d * d * uSharpness)`.
- **Parameters**: `uFreqX/Y` (1–20), `uPhase` (0–2π), `uAmplitude` (0–1),
  `uThickness` (0.002–0.02), `uGlow` (0–3), `uColor`, `uMode` (waveform/lissajous).
- **BPM hook**: `uPhase` advances by `beat * 2π` for beat-locked figure rotation.
- **Reference**: https://mondniles.com/en/tools/oscilloscope

---

### TIER 2 — Implement next (require ping-pong RenderTarget utility)

> **Infrastructure needed first**: A `PingPongTarget` helper that wraps two
> `THREE.WebGLRenderTarget`, a full-screen blit material, and exposes
> `read`, `write`, and `swap()` — ~40 lines. Then shaders below use a
> `uPrevFrame: { value: pingPong.read.texture }` uniform.

---

**8. FluidSim — Stable Navier-Stokes fluid simulation**
Difficulty: Hard

- **Visual**: Ink-in-water / smoke dynamics. Colors advect and diffuse. Applying
  forces on beat creates dramatic swooshes of color.
- **Algorithm**: Jos Stam's Stable Fluids on GPU. Three ping-pong passes per frame:
  1. Advection (semi-Lagrangian: read velocity field at `uv - vel*dt`, write new vel)
  2. Diffusion (Jacobi iteration × 20 steps)
  3. Pressure projection (divergence → pressure solve → subtract gradient)
  Color is a fourth texture advected by the final velocity field.
- **Parameters**: `uViscosity` (0–1), `uDiffusion` (0–1), `uForceRadius` (0.01–0.2),
  `uForceStrength` (0–5), `uColorA/B/C` (injection colors), `uDecay` (0.95–1.0).
- **BPM hook**: Inject a radial force burst at beat position, color-matched to BPM.
- **Reference**: https://github.com/amandaghassaei/VortexShedding,
  https://www.shadertoy.com/view/l3tfz4

---

**9. ReactionDiffusion — Gray-Scott two-chemical system**
Difficulty: Hard

- **Visual**: Turing spots, stripes, labyrinths, and coral-like growths that
  self-organize in real time. Mesmerizing and unique in live shows.
- **Algorithm**: Two chemicals U and V stored in RG channels of ping-pong texture.
  Per frame: `U' = U + (Du * laplacian(U) - U*V² + f*(1-U)) * dt`
  and `V' = V + (Dv * laplacian(V) + U*V² - (f+k)*V) * dt`.
  `f` (feed) and `k` (kill) parameters select which pattern emerges.
- **Parameters**: `uFeed` (0.01–0.1), `uKill` (0.04–0.07), `uDu` (0.2–1.0),
  `uDv` (0.05–0.5), `uColorA/B` (chemical A / B colors), `uSeed` (inject pulse).
- **BPM hook**: Inject U=0 V=1 circle at beat center for pattern explosions.
- **Reference**: https://github.com/jasonwebb/reaction-diffusion-playground,
  https://tympanus.net/codrops/2024/05/01/reaction-diffusion-compute-shader-in-webgpu/

---

**10. FeedbackBloom — Frame feedback with zoom/rotate decay**
Difficulty: Medium

- **Visual**: Infinite-zoom trails, time-echo, psychedelic smearing. Every frame
  samples the previous frame slightly zoomed/rotated, then composites the current
  layer on top. Creates lush visual memory.
- **Algorithm**:
  ```glsl
  vec2 feedUv = (vUv - 0.5) * uZoomFactor + 0.5 + uDrift;
  feedUv = rotate2D(feedUv - 0.5, uRotateFactor * uTime) + 0.5;
  vec4 prev = texture2D(uPrevFrame, feedUv) * uDecay;
  vec4 curr = /* current layer output */;
  gl_FragColor = prev + curr;
  ```
- **Parameters**: `uDecay` (0.9–0.99), `uZoomFactor` (0.995–1.005),
  `uRotateFactor` (–0.01–0.01), `uDrift` (vec2, –0.002–0.002),
  `uColorShift` (hue-rotate on feedback loop).
- **BPM hook**: Momentary `uDecay` drop on beat clears the trail for rhythmic pulsing.

---

**11. GameOfLife — Conway's Cellular Automaton (stylized)**
Difficulty: Medium

- **Visual**: Glowing cells that live and die by neighbor rules. At show-scale
  with colored cells, glow, and additive blending this looks alien and hypnotic.
- **Algorithm**: Ping-pong stores cell state in R channel (1=alive, 0=dead).
  Per pixel, sample 8 neighbors in previous frame, count live ones, apply B3/S23
  rules. Render: alive cells emit `uCoreColor`; apply distance-field glow bloom
  (a simple 3×3 blur pass).
- **Parameters**: `uCellSize` (2–16 px), `uColorAlive`, `uColorGlow`,
  `uGlowRadius` (1–8), `uSpawnRate` (0–0.01, replenish dead regions),
  `uResetOnBeat` (bool).
- **BPM hook**: On beat, flip a random 32×32 block of cells for rhythmic chaos.
- **Reference**: https://nullprogram.com/blog/2014/06/10/,
  https://tympanus.net/codrops/2022/11/25/conways-game-of-life-cellular-automata-and-renderbuffers-in-three-js/

---

### TIER 3 — High-impact, moderate–hard

---

**12. RaymarchMetaballs — SDF metaball scene with smooth union**
Difficulty: Hard

- **Visual**: Glowing organic blobs that merge and separate. Extremely popular in
  live show rider decks — looks 3D with zero geometry cost.
- **Algorithm**: Classic sphere-trace with a smooth-min SDF for N metaballs:
  ```glsl
  float smin(float a, float b, float k) {
    float h = clamp(0.5 + 0.5*(b-a)/k, 0.0, 1.0);
    return mix(b, a, h) - k*h*(1.0-h);
  }
  float scene(vec3 p) {
    float d = 1e9;
    for (int i = 0; i < 6; i++) d = smin(d, sdSphere(p, centers[i], radii[i]), uBlend);
    return d;
  }
  ```
  Use a simple orthographic ray (no camera projection needed) since the canvas is
  already flat. Cheap AO: sample scene() at `p + N*eps`.
- **Parameters**: `uBallCount` (2–8), `uBlend` (0.1–1.0), `uSpeed` (0–2),
  `uOrbitRadius` (0.1–0.8), `uCoreColor`, `uGlowColor`, `uGlowFalloff` (1–5).
- **BPM hook**: `uBlend` pulses on beat (blobs merge on downbeat).
- **Reference**: https://jamie-wong.com/2016/07/06/metaballs-and-webgl/,
  https://iquilezles.org/articles/smin/

---

**13. FractalJulia — Julia Set with animated parameter**
Difficulty: Medium

- **Visual**: Endlessly complex fractal boundary, slow zoom and parameter morph.
  Works well as a background layer with additive blending.
- **Algorithm**: Standard complex iteration in GLSL. Animate the Julia constant `c`
  around a small orbit to keep it on the boundary:
  ```glsl
  vec2 c = vec2(cos(uTime * uOrbitSpeed) * uOrbitR + uCX,
                sin(uTime * uOrbitSpeed) * uOrbitR + uCY);
  vec2 z = (vUv - 0.5) * uZoom;
  for (int i = 0; i < uMaxIter; i++) {
    z = vec2(z.x*z.x - z.y*z.y, 2.0*z.x*z.y) + c;
    if (dot(z,z) > 4.0) { /* color by escape iter */ break; }
  }
  ```
  Color with the smooth iteration count and a cosine palette.
- **Parameters**: `uCX/CY` (–1.0–1.0, Julia constant), `uOrbitR/Speed` (animated c),
  `uZoom` (0.5–3), `uMaxIter` (32–256), `uColorA/B/C/D` (palette).
- **Note**: Use `mediump` for speed; precision breaks only at deep zoom.
- **Reference**: https://elijah.mirecki.com/blog/glsl-mandelbrot/

---

**14. AuroraCurtain — Volumetric aurora bands**
Difficulty: Hard

- **Visual**: Sweeping curtains of green/teal/purple light that wave like real
  auroras. Jaw-dropping on wide screens.
- **Algorithm**: 2D layered FBM "extruded" vertically to fake volumetric columns.
  Multiple horizontal bands at different heights, each animated by FBM displacement:
  ```glsl
  float aurora(vec2 uv, float seed) {
    float col = 0.0;
    for (int i = 0; i < 8; i++) {
      float fi = float(i);
      float y = fbm(vec2(uv.x * 2.0 + fi + uTime * 0.1, seed + fi)) - uv.y;
      col += exp(-abs(y) * uSharpness) * (0.5 + 0.5 * fbm(vec2(uv.x + uTime*0.05, seed)));
    }
    return col;
  }
  ```
- **Parameters**: `uSharpness` (5–30), `uSpeed` (0–0.5), `uHeight` (0.3–0.8),
  `uColorGreen`, `uColorMagenta`, `uColorBlue` (band tints), `uBands` (1–4, int).
- **Reference**: https://www.shadertoy.com/view/XtGGRt (nimitz),
  https://blog.roytheunissen.com/2022/09/17/aurora-borealis-a-breakdown/

---

**15. GPUParticleAttractor — Compute-style GPU particles via ping-pong float textures**
Difficulty: Hard

- **Visual**: Millions of particles pulled toward beat-driven attractor points,
  leaving luminous trails. Think of galaxies forming in real time.
- **Algorithm**: Store particle positions and velocities in two RGBA float textures
  (ping-pong). Simulation shader reads position/velocity, applies attractor force
  `F = normalize(attractor - pos) * uStrength / (d² + epsilon)` plus noise, writes
  back. Render shader draws each texel as a point sprite (reuse `ParticleShader`
  gl_PointSize logic).
- **Parameters**: `uParticleCount` (4096–65536, power of 2), `uAttractorCount` (1–4),
  `uStrength` (0–5), `uNoiseScale` (0–2), `uDecay` (0.98–1.0 velocity damping),
  `uPointSize` (1–4), `uColorBySpeed` (bool).
- **Note**: Three.js `DataTexture` with `THREE.RGBAFormat`, `THREE.FloatType` for
  particle state. This replaces the CPU loop in `ParticleShader` entirely.
- **BPM hook**: Snap attractor position to a new random point on beat.
- **Reference**: https://dev.to/hexshift/building-a-custom-gpu-accelerated-particle-system-with-webgl-and-glsl-shaders-25d2

---

**16. GridDistort — Pixel-grid displacement with edge highlight**
Difficulty: Easy–Medium

- **Visual**: The frame gets subdivided into a grid; each cell is independently
  displaced, scaled, and rotated by noise. Looks like a digital glitch mosaic or
  data corruption. Strong on top of logo masks.
- **Algorithm**:
  ```glsl
  vec2 cell = floor(vUv * uGridRes) / uGridRes;
  float n = fbm(cell * uNoiseScale + uTime * uSpeed);
  vec2 displaced = vUv + vec2(n - 0.5, fbm(cell + 7.3 + uTime * 0.5) - 0.5) * uMagnitude;
  vec4 col = texture2D(uSource, fract(displaced));
  ```
  Add cell-boundary highlight by detecting `fract(vUv * uGridRes) < uBorder`.
- **Parameters**: `uGridRes` (4–64), `uMagnitude` (0–0.3), `uSpeed` (0–2),
  `uNoiseScale` (0.5–5), `uBorder` (0–0.1), `uBorderColor`, `uRotate` (0–1 per-cell).

---

**17. ScanlineRetro — CRT scan-line + phosphor glow post-process**
Difficulty: Easy

- **Visual**: Classic CRT monitor look — horizontal scanlines, barrel distortion,
  phosphor bloom. Instant retro identity for chiptune or synth acts.
- **Algorithm**:
  ```glsl
  // Barrel distortion
  vec2 uv2 = vUv - 0.5;
  uv2 *= 1.0 + dot(uv2, uv2) * uBarrel;
  uv2 += 0.5;
  // Scanlines
  float scan = sin(uv2.y * uResolution.y * PI) * 0.5 + 0.5;
  scan = pow(scan, uScanPow);
  vec4 col = texture2D(uSource, uv2) * (uScanIntensity * scan + (1.0 - uScanIntensity));
  // Vignette
  col *= 1.0 - dot(uv2 - 0.5, uv2 - 0.5) * uVignette;
  ```
- **Parameters**: `uBarrel` (0–0.3), `uScanIntensity` (0–1), `uScanPow` (0.5–4),
  `uVignette` (0–3), `uBrightness`, `uFlicker` (0–0.05 random frame flicker).

---

## Architecture Suggestions

1. **Add `PingPongTarget` utility** (`src/renderer/PingPongTarget.js`). Wrap two
   `THREE.WebGLRenderTarget` instances with a `swap()` method and a standard
   full-screen blit material. Shaders 8–11 and 15 all depend on this. Estimated
   effort: 1–2 hours. Returns enormous value.

2. **Pass `uBeat` and `uBPM` uniforms to all shaders**. The BPM clock is already
   available in `update()` but no shader uses it. Add `uBeat: { value: 0 }` and
   `uBPM: { value: 120 }` to the base uniform set in `LayerManager.update()` and
   inject them automatically. This is the biggest untapped expressive lever.

3. **Add a "source layer" input** — a way for post-process shaders (ChromaGlitch,
   ScanlineRetro, FeedbackBloom) to sample the rendered output of another layer.
   Implement by rendering the source layer to an intermediate `WebGLRenderTarget`
   and passing its texture as `uSource`. A `SourceLayer` property on the GUI
   dropdown (listing current layer names) would drive this.

4. **Shader hot-reload**: Since the GLSL lives in `.js` string literals, consider
   moving each shader's fragment source to a `.frag` file and loading it via
   Electron's `fs.readFile`. Then a file-watcher can trigger recompile without
   restarting the app — critical for live shader editing sessions.

5. **Preset save/load**: Serialize all layer `params` objects to JSON via
   `ipcRenderer.invoke('dialog:save')`. Performers need to recall a rig state
   instantly at show time.

6. **MIDI input routing**: Map any GUI parameter to a MIDI CC via a learn mode
   (click param → move knob). The lil-gui `onChange` callbacks make this
   straightforward. ISF and Synesthesia both treat MIDI as first-class.

7. **Audio FFT input**: Add a `uFFT` texture (`THREE.DataTexture`, 512×1,
   `LuminanceFormat`) updated from the Web Audio API `AnalyserNode` each frame.
   Pass it to all shaders. PlasmaWave, OscilloscopeWave, and the particle attractor
   particularly benefit. This is what separates SpaceGen from a screensaver.

---

## Reference Links

| Resource | URL |
|---|---|
| Inigo Quilez — Domain Warping | https://iquilezles.org/articles/warp/ |
| Inigo Quilez — Cosine Palettes | https://iquilezles.org/articles/palettes/ |
| Inigo Quilez — Smooth Min (smin) | https://iquilezles.org/articles/smin/ |
| Inigo Quilez — FBM | https://iquilezles.org/articles/fbm/ |
| Inigo Quilez — Raymarching SDFs | https://iquilezles.org/articles/raymarchingdf/ |
| Book of Shaders — Noise | https://thebookofshaders.com/11/ |
| Book of Shaders — Fractal Brownian Motion | https://thebookofshaders.com/13/ |
| Book of Shaders — Cellular Noise | https://thebookofshaders.com/12/ |
| Shadertoy — Domain Warped FBM (nimitz) | https://www.shadertoy.com/view/wttXz8 |
| Shadertoy — Aurora (nimitz) | https://www.shadertoy.com/view/XtGGRt |
| Shadertoy — Navier-Stokes Fluid | https://www.shadertoy.com/view/l3tfz4 |
| Shadertoy — Tunnel Effect | https://www.shadertoy.com/view/4djBRm |
| ISF Shader Collection (200+ shaders) | https://github.com/Vidvox/ISF-Files |
| ISF Audio Visualizer Primer | https://docs.isf.video/primer_chapter_8.html |
| ISF Editor (live GLSL browser) | https://editor.isf.video/ |
| Reaction Diffusion Playground (jasonwebb) | https://github.com/jasonwebb/reaction-diffusion-playground |
| Reaction Diffusion — WebGPU Compute (Codrops) | https://tympanus.net/codrops/2024/05/01/reaction-diffusion-compute-shader-in-webgpu/ |
| Conway Game of Life — GPU (nullprogram) | https://nullprogram.com/blog/2014/06/10/ |
| Game of Life — Three.js (Codrops) | https://tympanus.net/codrops/2022/11/25/conways-game-of-life-cellular-automata-and-renderbuffers-in-three-js/ |
| Metaballs & WebGL (Jamie Wong) | https://jamie-wong.com/2016/07/06/metaballs-and-webgl/ |
| GPU Particle System (WebGL/GLSL) | https://dev.to/hexshift/building-a-custom-gpu-accelerated-particle-system-with-webgl-and-glsl-shaders-25d2 |
| Amanda Ghassaei — WebGL Shaders | https://amandaghassaei.com/projects/shaders/ |
| Audio Reactive Shaders — Three.js/Shader Park | https://tympanus.net/codrops/2023/02/07/audio-reactive-shaders-with-three-js-and-shader-park/ |
| Shadertoy FFT Details | https://gist.github.com/soulthreads/2efe50da4be1fb5f7ab60ff14ca434b8 |
| Plasma Effect Reference | https://www.bidouille.org/prog/plasma |
| Electric Square — Raymarching Workshop | https://github.com/electricsquare/raymarching-workshop |
| GLSL Noise Algorithms (Patricio G.V.) | https://gist.github.com/patriciogonzalezvivo/670c22f3966e662d2f83 |
| Awesome GLSL Resources (curated list) | https://github.com/vanrez-nez/awesome-glsl |
| Synesthesia Shader Format (SSF) | https://app.synesthesia.live/docs/ssf/ssf.html |
| Synesthesia GLSL Resources | https://app.synesthesia.live/docs/resources/glsl_resources.html |
| VJ Zef — Saturday Shaders (VDMX) | https://vdmx.vidvox.net/blog/vj-zef-saturday-shaders |
| Aurora Borealis Breakdown (Roy Theunissen) | https://blog.roytheunissen.com/2022/09/17/aurora-borealis-a-breakdown/ |
| Tileable Procedural Shaders (tuxalin) | https://github.com/tuxalin/procedural-tileable-shaders |
| Oscilloscope Music Visualizer (browser) | https://mondniles.com/en/tools/oscilloscope |
