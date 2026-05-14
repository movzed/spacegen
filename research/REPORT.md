# SpaceGen Research Report
> Maintained by the Research Team. Engineering reads this daily.

---

## Status
- [ ] Live show tools survey (in progress)
- [ ] Shader library audit (in progress)
- [ ] Priority proposals for engineering (pending)

---

## Findings will appear below as research agents complete their work.

---

## Shader Research (Research Team — Shaders Agent)

> Completed: 2026-05-14. Sources: Shadertoy, iquilezles.org, ISF community (isf.video / GitHub), The Book of Shaders, Synesthesia docs, Three.js ecosystem.

---

### 1. Plasma / Liquid / Lava Lamp Effects

**Description**  
Classic plasma effects blend sine waves across UV space to produce smoothly morphing color fields. Lava lamp variants use signed-distance blobs (metaballs) or smooth-min SDF operations to simulate buoyant blobs rising and merging.

**Algorithm outline (2D plasma)**
```glsl
// Classic sine plasma
vec2 uv = fragCoord / iResolution.xy;
float t = iTime * 0.5;
float v = sin(uv.x * 10.0 + t);
v    += sin(uv.y * 10.0 + t * 1.3);
v    += sin((uv.x + uv.y) * 8.0 + t * 0.7);
v    += sin(length(uv - 0.5) * 12.0 - t * 2.0);
// map v → cosine palette
vec3 col = palette(v * 0.5 + 0.5, a, b, c, d);
```

**Algorithm outline (SDF metaball lava lamp)**
```glsl
// smooth-min merge of N spheres in a ray-march loop
float smin(float a, float b, float k) {
    float h = clamp(0.5 + 0.5*(b-a)/k, 0.0, 1.0);
    return mix(b, a, h) - k*h*(1.0-h);
}
float scene(vec3 p) {
    float d = 1e9;
    for (int i = 0; i < N_BLOBS; i++) {
        d = smin(d, length(p - blobPos[i]) - blobR[i], 0.4);
    }
    return d;
}
```

**Visual character:** molten, organic, slow-breathing. Perfect loop background or layer.  
**Why compelling for live shows:** Zero geometry, pure math. Responds instantly to BPM-driven time-scale. Layer color palette on a beat trigger for instant mood shift.

**Shadertoy references**
- Lava lamp blobs — https://www.shadertoy.com/view/ltdBR4
- Raymarched Lava Lamp — https://www.shadertoy.com/view/4tGfzm
- Ray-marched lava lamp shader — https://www.shadertoy.com/view/7lfBRj
- SDF sphere-marched lava lamp — https://www.shadertoy.com/view/fdVcRm
- Fluid Lava Lamp — https://www.shadertoy.com/view/dscBDn
- [2TC 15] Lava Lamp — https://www.shadertoy.com/view/Xts3WH

---

### 2. Voronoi / Cell Patterns

**Description**  
Voronoi diagrams partition space by nearest-point distance. In GLSL the standard approach is a 3×3 (or 2×2 Gustavson-optimized) grid search. Extensions: F2-F1 for cell borders, smooth-Voronoi for organic edges, Voronoise (parametric blend between Perlin and Voronoi).

**Algorithm outline**
```glsl
// Standard 2D Voronoi (Inigo Quilez)
vec2 voronoi(vec2 x) {
    vec2 p = floor(x);
    vec2 f = fract(x);
    float minDist = 8.0;
    vec2 minId = vec2(0.0);
    for (int j = -1; j <= 1; j++)
    for (int i = -1; i <= 1; i++) {
        vec2 b = vec2(float(i), float(j));
        vec2 r = b - f + hash2(p + b);   // hash2 → pseudo-random [0,1]^2
        float d = dot(r, r);
        if (d < minDist) { minDist = d; minId = p + b; }
    }
    return vec2(minDist, dot(minId, vec2(1.0, 37.0)));
}

// F2 - F1 border mask
float border = smoothstep(0.0, 0.08, abs(f2Dist - f1Dist));
```

**Visual character:** cracked glass, cells, organic topology, crystal lattice. Animates by slowly moving feature points.  
**Why compelling:** Extremely controllable. Animated cell centroids → pulsing beat reaction. Border highlight → neon grid feel. Pair with palette cycling for instant color theming.

**Shadertoy references**
- Voronoi - basic — https://www.shadertoy.com/view/MslGD8 (iq)
- Voronoi - distances — https://www.shadertoy.com/view/ldl3W8 (iq)
- Voronoise — https://www.shadertoy.com/view/Xd23Dh (iq)
- Voronoi - 3D — https://www.shadertoy.com/view/ldl3Dl (iq)
- Faster Voronoi Edge Distance — https://www.shadertoy.com/view/llG3zy
- Quasi Infinite Zoom Voronoi (ISF) — https://github.com/grigM/ISF-shaders-collection/blob/master/Quasi_Infinite_Zoom_Voronoi__XlBXWw.fs

---

### 3. Ray Marching: Tunnels, Fractals, Kaleidoscopes

**Description**  
Ray marching with Signed Distance Functions (SDFs) allows rendering complex 3D geometry entirely in a fragment shader. A ray is stepped forward by the SDF value at each position (sphere tracing). Tunnels are SDF cylinders with repeated modular space. Fractals use IFS (Iterated Function System) folds. Kaleidoscopes layer polar mirror symmetry on top of ray-marched scenes.

**Algorithm outline (sphere-trace loop)**
```glsl
float rayMarch(vec3 ro, vec3 rd) {
    float t = 0.0;
    for (int i = 0; i < MAX_STEPS; i++) {
        float d = sceneSDF(ro + rd * t);
        if (d < SURF_DIST) break;
        if (t > MAX_DIST)  break;
        t += d;
    }
    return t;
}
```

**Algorithm outline (infinite tunnel)**
```glsl
float sceneSDF(vec3 p) {
    // repeat along Z
    p.z = mod(p.z + iTime * speed, tunnelPeriod) - tunnelPeriod * 0.5;
    // add twist
    float angle = p.z * twistRate;
    p.xy = mat2(cos(angle), -sin(angle), sin(angle), cos(angle)) * p.xy;
    return tunnelRadius - length(p.xy);   // hollow cylinder SDF (negative inside)
}
```

**Algorithm outline (kaleidoscope fold)**
```glsl
// Polar mirror — applied before ray-marching
vec2 kaleido(vec2 uv, float n) {
    float angle = atan(uv.y, uv.x);
    float sector = TAU / n;
    angle = mod(angle, sector);
    if (angle > sector * 0.5) angle = sector - angle;  // mirror
    float r = length(uv);
    return r * vec2(cos(angle), sin(angle));
}
```

**Visual character:** Tunnels give infinite-zoom rush, perfect for drops. Kaleidoscopes produce mandala geometry at any tempo. Fractals (Mandelbulb, Mandelbox, Kali sets) give alien landscape feel.  
**Why compelling:** All GPU, no assets, infinite variation from parameter tweaks.

**Shadertoy references**
- Raymarching - Primitives (iq reference) — https://www.shadertoy.com/view/Xds3zN
- Tunnel Effect Shader — https://www.shadertoy.com/view/4djBRm
- Fractal Land (Kali) — https://www.shadertoy.com/view/XsBXWt
- Everyday 081 - Apollonian Tunnel — https://www.shadertoy.com/view/t3ffRN
- Basic: Apollonian Gaskets — https://www.shadertoy.com/view/3sSyDG
- Ray Marching Fractals — https://www.shadertoy.com/view/fslGR8
- Protean clouds — https://www.shadertoy.com/view/3l23Rh
- Seascape (Alexander Alekseev) — https://www.shadertoy.com/view/Ms2SD1
- Clouds (iq) — https://www.shadertoy.com/view/XslGRr
- kaleidoscope shader — https://www.shadertoy.com/view/tfdXRS
- simple kaleidoscope — https://www.shadertoy.com/view/4f33Dl
- LYGIA kaleidoscope — https://lygia.xyz/space/kaleidoscope

---

### 4. Feedback / Echo Effects (Ping-Pong Render Targets)

**Description**  
Feedback loops read the previous frame's texture (or a multipass buffer) as input to the current frame. Small zoom, rotation, or color shifts accumulate into trails, smearing, blooming echoes, and reaction-diffusion patterns. In Three.js this maps directly to two `WebGLRenderTarget` objects swapped each frame (ping-pong).

**Algorithm outline**
```glsl
// Buffer A: read previous frame, apply transform, decay
uniform sampler2D prevFrame;
void main() {
    vec2 uv = fragCoord / iResolution.xy;
    // slight zoom toward center (feedback zoom)
    vec2 feedback_uv = (uv - 0.5) * 0.995 + 0.5;
    // optionally rotate
    // feedback_uv = rotate(feedback_uv, 0.002);
    vec4 prev = texture2D(prevFrame, feedback_uv);
    // decay / desaturate to prevent blow-out
    prev.rgb *= 0.97;
    // add new content
    vec4 current = renderCurrentContent(uv);
    gl_FragColor = prev + current;
}
```

**Three.js ping-pong setup**
```js
const rtA = new THREE.WebGLRenderTarget(w, h, { type: THREE.HalfFloatType });
const rtB = rtA.clone();
// Each frame:
renderer.setRenderTarget(rtB);
quad.material.uniforms.prevFrame.value = rtA.texture;
renderer.render(scene, camera);
[rtA, rtB] = [rtB, rtA];  // swap
```

**Visual character:** trails that slowly decay, echo smears, recursive zoom tunnels, infinite mirror halls. Gray-Scott reaction-diffusion patterns emerge from two-chemical feedback.  
**Why compelling:** Organic complexity from minimal code. Beat-driven "inject" pulses create satisfying accent moments.

**Shadertoy references**
- shader in the loop video feedback — https://www.shadertoy.com/view/4sKBDc
- Interactive Video Feedback #2 — https://www.shadertoy.com/view/l3jSzW
- Feedback Scope — https://www.shadertoy.com/view/ttlGR2
- expansive reaction-diffusion — https://www.shadertoy.com/view/4dcGW2
- Gray Scott model - Fragile life — https://www.shadertoy.com/view/lXXcz7
- Gray Scott Reaction Diffusion 2 — https://www.shadertoy.com/view/WdlcWM

---

### 5. Glitch / Datamosh / Pixel Displacement

**Description**  
Glitch effects simulate video codec artifacts: block displacement, chromatic aberration, scan-line tearing, RGB channel offset. Datamosh extends this with motion-vector feedback — pixels from the previous frame are displaced using an estimated or procedural flow field rather than replaced with new data, producing "frozen P-frame" smear.

**Algorithm outline (glitch bands)**
```glsl
float glitchBand(vec2 uv, float t) {
    float line   = floor(uv.y * 30.0);
    float seed   = fract(sin(line * 127.1 + t * 23.0) * 43758.5);
    float active = step(0.92, seed);   // ~8% of rows are glitched
    float shift  = (fract(sin(seed * 94.1) * 3758.0) - 0.5) * 0.08;
    return active * shift;
}
void main() {
    vec2 uv = fragCoord / iResolution.xy;
    float dx = glitchBand(uv, iTime);
    // chromatic aberration
    float r = texture2D(tex, uv + vec2(dx + 0.003, 0.0)).r;
    float g = texture2D(tex, uv + vec2(dx,          0.0)).g;
    float b = texture2D(tex, uv + vec2(dx - 0.003, 0.0)).b;
    gl_FragColor = vec4(r, g, b, 1.0);
}
```

**Algorithm outline (datamosh / optical flow feedback)**
```glsl
// Pass 1: compute motion estimate (difference of frames)
vec2 motionVec = texture2D(curr, uv).rg - texture2D(prev, uv).rg;
// Pass 2: displace prev frame by accumulated motion
vec2 displaced_uv = uv + motionVec * moshStrength;
gl_FragColor = texture2D(prevFrame, displaced_uv);
```

**Visual character:** corrupted broadcast, VHS smear, cyberpunk break. High-energy accent effect.  
**Why compelling:** Instantly reads as "digital" chaos. Controllable intensity — from subtle grime to full meltdown. Great on a snare or kick accent layer.

**Shadertoy references**
- Data Moshing Effect — https://www.shadertoy.com/view/tlsSRs
- Glitchy Glitch — https://www.shadertoy.com/view/wld3WN
- Optical flow datamosh technique — https://cdm.link/liquidify-video-live-optical-flow-glsl-datamosh-technique/

---

### 6. Domain Warping with fBm (Inigo Quilez)

**Description**  
Domain warping applies fBm (Fractional Brownian Motion) as a displacement to the input coordinates of another fBm call, creating self-similar, turbulent, organic shapes: marble veins, smoke, clouds, lava flows. Multi-level nesting (q = f(p), r = f(p + q), result = f(p + r)) dramatically increases complexity.

**Algorithm outline**
```glsl
// fBm primitive
float fbm(vec2 p) {
    float v = 0.0, a = 0.5;
    for (int i = 0; i < OCTAVES; i++) {
        v += a * noise(p);
        p  = m * p * 2.0;  // rotation matrix m prevents axis-alignment
        a *= 0.5;
    }
    return v;
}

// Domain warp (iq's formulation)
vec2 q = vec2(fbm(p + vec2(0.0, 0.0)),
              fbm(p + vec2(5.2, 1.3)));

vec2 r = vec2(fbm(p + 4.0*q + vec2(1.7, 9.2)),
              fbm(p + 4.0*q + vec2(8.3, 2.8)));

float value = fbm(p + 4.0*r);
```

**Visual character:** burning lava, roiling smoke, marble stone, alien weather systems. Infinitely zoomable.  
**Why compelling:** Single-pass, no geometry. Animating `p + iTime * 0.1` gives seamless looping drift. BPM-clock the amplitude of q or r for beat-reactive turbulence.

**Key references**
- iq article — https://iquilezles.org/articles/warp/
- Domain warped FBM noise — https://www.shadertoy.com/view/wttXz8
- Simple Domain Warping — https://www.shadertoy.com/view/wscSRj
- Book of Shaders fBm — https://thebookofshaders.com/13/
- GLSL Noise Algorithms collection — https://gist.github.com/patriciogonzalezvivo/670c22f3966e662d2f83

---

### 7. Noise Fields: Simplex, Worley, fBm, Curl

**Description**  
Four noise types cover most live-visual needs:

| Noise | Characteristics | Best use |
|-------|----------------|----------|
| Simplex (Gustavson / McEwan) | Fast, no axis artifacts, analytical gradient | Base layer, smooth morph |
| Worley (cellular) | F1 = crystal cells, F2-F1 = borders | Voronoi overlays, skin texture |
| fBm | Octave sum of Simplex/Value noise | Terrain, clouds, flame |
| Curl | Divergence-free from noise gradient | Fluid-like particle flow |

**Algorithm outlines**

```glsl
// Curl noise for particle flow field
vec3 curlNoise(vec3 p) {
    const float e = 0.1;
    // finite-difference gradient of noise
    float dx = noise(p + vec3(e,0,0)) - noise(p - vec3(e,0,0));
    float dy = noise(p + vec3(0,e,0)) - noise(p - vec3(0,e,0));
    float dz = noise(p + vec3(0,0,e)) - noise(p - vec3(0,0,e));
    // curl = rot of gradient
    return normalize(vec3(dy - dz, dz - dx, dx - dy));
}
```

```glsl
// Worley (F1 cellular) – tight inner loop
float worley(vec2 p) {
    vec2 ip = floor(p); float minD = 9.0;
    for (int y = -1; y <= 1; y++)
    for (int x = -1; x <= 1; x++) {
        vec2 cell = ip + vec2(x, y);
        vec2 pt   = cell + hash2(cell);
        minD = min(minD, length(p - pt));
    }
    return minD;
}
```

**Why compelling:** Curl noise drives GPU particle systems that look like fluid without any physics solver. Worley gives instant organic biology feel. fBm is the universal "nature" texture.

**Key references**
- Simplex GLSL — https://stegu.github.io/webgl-noise/webdemo/
- ashima/webgl-noise (zero-texture simplex) — https://github.com/ashima/webgl-noise
- glsl-worley — https://github.com/Erkaman/glsl-worley
- LYGIA noise library — https://lygia.xyz
- GLSL Noise Algorithms gist — https://gist.github.com/patriciogonzalezvivo/670c22f3966e662d2f83

---

### 8. Color Palette Cycling (Cosine Palette)

**Description**  
Inigo Quilez's cosine palette maps a scalar `t ∈ [0,1]` to RGB using four vec3 parameters. Animating the phase parameter `d` at any rate cycles through the full palette. This is the standard technique for colorizing noise, SDFs, and distance fields in live contexts.

**GLSL — exact formula**
```glsl
// a = brightness, b = contrast, c = frequency, d = phase offset
vec3 palette(float t, vec3 a, vec3 b, vec3 c, vec3 d) {
    return a + b * cos(6.28318 * (c * t + d));
}

// Example presets:
// Psychedelic:  a=0.5, b=0.5, c=1.0, d=vec3(0.0, 0.33, 0.67)
// Fire:         a=vec3(0.5,0.2,0.0), b=vec3(0.5,0.3,0.1), c=1.0, d=vec3(0.0,0.5,0.5)
// Ice:          a=0.5, b=0.5, c=vec3(1.0,1.0,0.5), d=vec3(0.8,0.9,0.3)

// Beat-reactive phase rotation:
float phase = iTime * bpmRate;   // e.g. bpmRate = BPM/60.0
vec3 color  = palette(noiseVal, a, b, c, d + vec3(phase));
```

**Why compelling:** One uniform change (`d`) shifts the entire color scheme instantly — ideal for a MIDI knob. Cycling at beat frequency creates pulsing color shifts that feel locked to music. Totally free of texture lookups.

**Key references**
- iq article — https://iquilezles.org/articles/palettes/
- Cosine color palette generator — https://www.shadertoy.com/view/tlSBDw
- Shader ll2GD3 (iq animated palette demo) — https://www.shadertoy.com/view/ll2GD3
- glsl-cos-palette npm — https://github.com/Erkaman/glsl-cos-palette

---

### 9. Chaser / Tracer / Saber / Energy Beam Effects

**Description**  
Chaser effects: a sequence of bright bands (or dots) that travel along a path over time, each with an exponential-decay glow. Saber/energy beam: an SDF-based bright line with a soft glow falloff using `exp(-d * k)` or `1.0/d` shaping. Beat injection fires a new tracer on each drum hit.

**Algorithm outline (chaser bands)**
```glsl
// 1D chaser along a parameterized path
float chaser(float t, float pos, float width, float decay) {
    float d = abs(fract(t - pos) - 0.5) * 2.0;  // periodic band
    return exp(-d * decay) * smoothstep(width, 0.0, d);
}

// Layer N chasers with time offsets
float glow = 0.0;
for (int i = 0; i < N; i++) {
    float offset = float(i) / float(N);
    glow += chaser(pathParam, iTime * speed + offset, 0.05, 8.0);
}
vec3 col = glowColor * glow;
```

**Algorithm outline (energy beam / saber)**
```glsl
// 2D SDF of a line segment → beam
float sdSegment(vec2 p, vec2 a, vec2 b) {
    vec2 pa = p-a, ba = b-a;
    float h = clamp(dot(pa,ba)/dot(ba,ba), 0.0, 1.0);
    return length(pa - ba*h);
}
float beam(vec2 uv, vec2 a, vec2 b) {
    float d = sdSegment(uv, a, b);
    float core  = smoothstep(0.003, 0.0,   d);
    float glow  = exp(-d * 20.0) * 0.6;
    float halo  = exp(-d * 5.0)  * 0.2;
    return core + glow + halo;
}
vec3 col = beamColor * beam(uv, start, end);
```

**Visual character:** Chasers: sequential light sweep, police lights, EQ bar animation. Sabers: lightsaber, laser, energy conduit.  
**Why compelling:** Immediate visual intensity on a snare or bass drop. Scalable: 1 beam = subtle accent; 16 beams = full rave strobing.

**Shadertoy references**
- Energy beam — https://www.shadertoy.com/view/7tlyzl
- Bidirectional Laser Tracer — https://www.shadertoy.com/view/7tjSDh
- GLOW TUTORIAL — https://www.shadertoy.com/view/3s3GDn
- Edge glow — https://www.shadertoy.com/view/Mdf3zr
- CRT Beam Simulator — https://www.shadertoy.com/view/43ccDN

---

### 10. GPU Particle Systems (GPGPU / FBO Particles)

**Description**  
GPU particle systems store particle state (position, velocity, life) in floating-point textures. Each frame a simulation shader updates the texture (reading position/vel, writing new values). A separate render shader draws each particle (as a billboard quad or point sprite) by sampling its texture pixel. In Three.js this uses `GPUComputationRenderer` or raw `WebGLRenderTarget` ping-pong. Millions of particles at 60fps are achievable.

**Architecture**
```
Frame N:
  [Simulation Shader] reads posTexA → writes posTexB
  [Velocity Shader]   reads velTexA → writes velTexB
  [Render Shader]     reads posTexB → draws instanced quads
Frame N+1: swap A/B
```

**Simulation shader (position update)**
```glsl
// Runs per-texel; each texel = one particle
uniform sampler2D posTexture;
uniform sampler2D velTexture;
uniform float dt;
uniform sampler2D curlField;   // pre-baked curl noise volume

void main() {
    vec2 uv   = gl_FragCoord.xy / resolution;
    vec3 pos  = texture2D(posTexture, uv).xyz;
    vec3 vel  = texture2D(velTexture, uv).xyz;
    
    // Curl noise steering
    vec3 curl = texture2D(curlField, uv).xyz * 2.0 - 1.0;
    vel       = mix(vel, curl * maxSpeed, 0.05);
    pos      += vel * dt;
    
    // respawn if out of bounds or age expired
    float age = texture2D(posTexture, uv).w + dt;
    if (age > lifespan) { pos = spawnPoint(uv); age = 0.0; vel = randomVel(uv); }
    
    gl_FragColor = vec4(pos, age);
}
```

**Three.js integration (R3F)**
```js
// Bruno Simon / Codrops GPGPU pattern
const gpgpu = new GPUComputationRenderer(SIZE, SIZE, renderer);
const posVar = gpgpu.addVariable('uParticles', simShader, initTex);
gpgpu.setVariableDependencies(posVar, [posVar]);
```

**Why compelling:** Visual density unachievable with CPU. Curl-noise-guided particles look like fire, smoke, liquid metal, galaxy spirals — all with zero CPU overhead. Beat-sync: inject velocity impulse from audio `iSoundEnergy` uniform.

**Key references**
- Three.js GPUComputationRenderer GPGPU — https://tympanus.net/codrops/2024/12/19/crafting-a-dreamy-particle-effect-with-three-js-and-gpgpu/
- Three.js Journey GPGPU lesson — https://threejs-journey.com/lessons/gpgpu-flow-field-particles-shaders
- FBO particles blog — https://barradeau.com/blog/?p=621
- ShaderParticleEngine — https://github.com/squarefeet/ShaderParticleEngine
- Magical World of Particles (R3F) — https://blog.maximeheckel.com/posts/the-magical-world-of-particles-with-react-three-fiber-and-shaders/

---

### 11. Beat-Reactive / BPM-Driven Animation

**Description**  
The pattern is: compute a clock signal from audio analysis or BPM tap, then drive shader parameters from that signal. Synesthesia's SSF format provides `syn_BPMSin`, `syn_BassTime`, `syn_HighHits`, `syn_RandomOnBeat` as built-in uniforms. In a custom WebGL engine (like SpaceGen), these must be computed on the JS side and uploaded as uniforms.

**BPM clock patterns**
```glsl
// Smooth sine pulse at BPM
float bpmPulse(float bpm, float t) {
    return 0.5 + 0.5 * sin(t * bpm / 60.0 * TAU);
}

// Sharp kick-drum envelope (fast attack, exp decay)
float kickEnvelope(float timeSinceKick) {
    return exp(-timeSinceKick * 8.0);
}

// 1/4 note sawtooth (ramps up then snaps)
float sawBeat(float bpm, float t) {
    return fract(t * bpm / 60.0);
}

// 1/8 note strobe gate
float strobeGate(float bpm, float t, float duty) {
    return step(duty, fract(t * bpm / 30.0));
}
```

**JS-side uniform upload**
```js
// In SpaceGen animation loop:
const beat = audioAnalyzer.getBeat();       // onset detection → 0 or 1
const level = audioAnalyzer.getLevel();     // 0..1 RMS
material.uniforms.uKick.value    = beat ? 1.0 : material.uniforms.uKick.value * 0.9;
material.uniforms.uLevel.value   = level;
material.uniforms.uBPMPhase.value = (Date.now() / 1000) * bpm / 60.0;
```

**Why compelling:** Sync is the #1 differentiator between "cool demo" and "live show visual." A correctly beat-locked shader feels like it _is_ the music.

**Key references**
- Synesthesia SSF docs — https://app.synesthesia.live/docs/ssf/ssf.html
- SSF Best Practices — https://app.synesthesia.live/docs/ssf/best_practices.html
- Audio Reactive Shaders Three.js + Shader Park (Codrops) — https://tympanus.net/codrops/2023/02/07/audio-reactive-shaders-with-three-js-and-shader-park/
- audio-shadertoy — https://github.com/audio-shadertoy/audio-shadertoy
- Chaolotus Sound Reactive — https://www.shadertoy.com/view/wstXW2

---

### 12. ISF (Interactive Shader Format) — VJ Ecosystem

**Description**  
ISF wraps GLSL in a JSON+GLSL file format with typed inputs (float, color, image, event, bool) and multi-pass support. Native in VDMX, Resolume Wire, MadMapper, Synesthesia, SMODE. ~200+ free shaders on `editor.isf.video`.

**Key ISF repositories for SpaceGen reference**
- Vidvox official ISF — https://github.com/Vidvox/isf
- Ethereios/ISF-shaders (41 fractal/3D shaders) — https://github.com/Ethereios/ISF-shaders
- bareimage/ISF (persistent buffers, animation-ready) — https://github.com/bareimage/ISF
- grigM/ISF-shaders-collection (includes Quasi Infinite Zoom Voronoi) — https://github.com/grigM/ISF-shaders-collection
- chimanaco/ISF-for-VDMX — https://github.com/chimanaco/ISF-for-VDMX
- isf.video browser — https://isf.video

**SpaceGen note:** ISF shaders are straightforward to port to Three.js `ShaderMaterial` — strip the JSON header, map ISF `inputs` to `uniforms`, replace `isf_FragNormCoord` with `vUv`, replace `IMG_THIS_PIXEL(image)` with `texture2D(uTex, vUv)`.

---

### 13. Three.js + GLSL: SpaceGen-Relevant Patterns

**Key architectural patterns for production live-show WebGL**

| Pattern | Implementation | Notes |
|---------|---------------|-------|
| Custom ShaderMaterial | `THREE.ShaderMaterial` with `vertexShader`/`fragmentShader` | Preferred over MeshStandardMaterial for full control |
| RawShaderMaterial | No THREE injections; cleanest for Shadertoy ports | Must manually add `precision mediump float;` |
| GPGPU particles | `GPUComputationRenderer` + ping-pong `WebGLRenderTarget` | Use `HalfFloatType` for compatibility |
| Postprocessing chain | `@react-three/postprocessing` or `EffectComposer` | Bloom → ChromaticAberration → Noise |
| Multipass FBO | Custom `WebGLRenderTarget` chain | Reaction-diffusion, feedback |
| useFBO (R3F) | Drei's `useFBO` hook | Simplest ping-pong in React context |
| Instanced geometry | `THREE.InstancedMesh` + per-instance GLSL | 100k+ meshes at 60fps |

**Converting Shadertoy to Three.js**
```glsl
// Shadertoy → Three.js uniform mapping:
// iTime          → uniform float uTime;
// iResolution    → uniform vec2  uResolution;
// iMouse         → uniform vec2  uMouse;
// fragCoord      → gl_FragCoord (same in GLSL ES)
// mainImage(out, in) → void main() { gl_FragColor = ...; }
```

**Key references**
- Three.js Journey — Shaders — https://threejs-journey.com/lessons/shaders
- Maxime Heckel shader study — https://blog.maximeheckel.com/posts/the-study-of-shaders-with-react-three-fiber/
- Codrops FBO postprocessing in R3F — https://dev.to/eriksachse/create-your-own-post-processing-shader-with-react-three-fiber-usefbo-and-dreis-shadermaterial-with-ease-1i6d
- Using Shadertoy shaders in Three.js — https://felixrieseberg.com/using-webgl-shadertoy-shaders-in-three-js/
- Three.js Shading Language (TSL, WebGPU-path) — https://github.com/mrdoob/three.js/wiki/Three.js-Shading-Language

---

## Top 10 Shaders to Implement Next in SpaceGen

Priority is based on: visual impact per GPU cost, audio-reactivity potential, uniqueness vs. stock VJ tools, and engineering complexity (lower = faster ship).

| # | Effect | Category | GPU Cost | Beat-React Potential | Eng. Complexity | Priority Score |
|---|--------|----------|----------|---------------------|-----------------|----------------|
| 1 | **Cosine Palette Cycling** | Color | Trivial | High (phase = BPM clock) | 1/5 | P0 |
| 2 | **Domain Warping fBm** | Noise/Warp | Low | High (amplitude of q/r) | 2/5 | P0 |
| 3 | **Voronoi + Border Glow** | Cell pattern | Low | High (cell point speed) | 2/5 | P0 |
| 4 | **2D Plasma (sine superposition)** | Plasma | Trivial | High (frequency scale) | 1/5 | P0 |
| 5 | **Feedback Zoom Loop** | Feedback | Low (ping-pong) | High (zoom rate = kick) | 3/5 | P1 |
| 6 | **GPGPU Curl Noise Particles** | Particles | Medium | High (vel impulse on beat) | 4/5 | P1 |
| 7 | **Ray-March Infinite Tunnel** | Ray march | Medium | High (twist rate, speed) | 3/5 | P1 |
| 8 | **Glitch / Chromatic Aberration** | Glitch | Low | High (intensity = snare) | 2/5 | P1 |
| 9 | **Chaser / Energy Beam** | Tracer | Low | Very high (N beams = BPM) | 2/5 | P2 |
| 10 | **Kaleidoscope (polar mirror on raymarched scene)** | Kaleidoscope | Medium | Medium (n-fold = param) | 3/5 | P2 |

### Implementation notes per priority

**P0 — Implement immediately (this sprint):**

1. **Cosine Palette** — Drop the 4-line `palette()` function into every existing shader's color output. Zero risk, instant visual upgrade. Wire `d.x` to a MIDI CC and `d.y` to beat phase.

2. **Domain Warp fBm** — Start from iq's formulation (wttXz8). Use Value noise for speed; replace with Simplex if artifacts appear. Key uniform: `uWarpAmp` (0→2) on a MIDI fader.

3. **Voronoi** — Port MslGD8 (iq basic) first. Add F2-F1 border mask with `smoothstep`. Animate centroids with `sin(iTime + hash)` offset. Beat: scale centroid velocity on kick.

4. **2D Plasma** — Four overlaid sine waves. Add to alpha layer compositor as a base fill. Use palette() for coloring. Simplest possible starting point.

**P1 — Next sprint:**

5. **Feedback loop** — Implement ping-pong in Three.js with two `WebGLRenderTarget(HalfFloat)`. Start with 0.995 zoom + 0.97 decay. Add rotation term (0.001 rad/frame). Beat: inject a bright flash into the accumulation buffer on kick.

6. **GPGPU Particles** — Use Bruno Simon's `GPUComputationRenderer` pattern. Start with 256×256 texture = 65k particles. Simulation: curl noise velocity + lifetime reset. Render: instanced `PlaneGeometry` billboard facing camera. Add BPM pulse to curl amplitude.

7. **Tunnel** — Start from Shadertoy 4djBRm (tunnel effect). Port to Three.js fullscreen quad. Add `uSpeed` uniform driven by `syn_BassTime` analog. Add SDF rings for visual interest.

8. **Glitch** — Layer on top of any scene as a postprocessing pass. Use `uGlitchIntensity` driven by snare hit envelope (exp decay). Chromatic aberration = `uIntensity * 0.01` max offset.

**P2 — Following sprint:**

9. **Chaser/Beam** — Implement as a 2D pass over any base layer. Stack 4–16 chaser lines parameterized along UV.y. Drive travel speed with BPM sawtooth. Width and brightness from audio level.

10. **Kaleidoscope** — Apply as a UV-space post-transform before any existing shader. Start with 6-fold polar mirror. Add slow rotation at `0.1 * bpmRate`. Allow n-fold to be MIDI-controlled (2–16).

---

## Useful Libraries and Tools

| Name | Purpose | URL |
|------|---------|-----|
| lygia.xyz | GLSL include library (noise, space, color) | https://lygia.xyz |
| ashima/webgl-noise | Zero-texture Simplex in GLSL | https://github.com/ashima/webgl-noise |
| glsl-worley | Worley cellular noise | https://github.com/Erkaman/glsl-worley |
| glsl-cos-palette | Cosine palette npm | https://github.com/Erkaman/glsl-cos-palette |
| Synesthesia SSF | Audio-reactive shader runtime | https://app.synesthesia.live/docs/ssf/ssf.html |
| isf.video | ISF shader browser/editor | https://isf.video |
| GPUComputationRenderer | Three.js GPGPU | Built into Three.js examples |
| shader-web-background | Shadertoy-compatible multipass JS | https://github.com/xemantic/shader-web-background |
| The Book of Shaders | Foundational GLSL reference | https://thebookofshaders.com |
| iquilezles.org | Authoritative SDF/noise/palette articles | https://iquilezles.org/articles/ |

---

*Research compiled by Shaders Agent, SpaceGen Research Team — 2026-05-14*

---

## Tool Survey (Research Team — Tools Agent)

> Researched: May 2026. Sources: Derivative.ca, Resolume.com, VDMX/Vidvox, Notch.one, vvvv.org, ISF.video, Shadertoy.com, Inigo Quilez, CDM, The Node Institute, Interactive & Immersive HQ, VJ Galaxy, VFX Voice, and others.

---

### Software Platform Deep-Dives

---

#### 1. TouchDesigner (Derivative)

**Category:** Node-based real-time programming environment  
**Platform:** Windows, macOS  
**License:** Commercial (free non-commercial tier available)  
**Website:** https://derivative.ca  

**Key Strengths:**
- Dataflow node graph ("operators") that wires TOPs (Texture Operators), CHOPs (Channel Operators), SOPs (Surface Operators), DATs, COMPs into complete pipelines.
- Arguably the most widely used platform for high-end interactive installation and VJ work. Dominant in the "build from scratch" tier — its user base skews toward engineers and generative artists, not clip-mixing DJs.
- Deep audio analysis via CHOP pipeline: FFT, onset detection, beat detection, amplitude envelopes — all routable directly to shader uniforms without any additional code.
- GPU particle systems: millions of particles simulated entirely on GPU. Feedback TOPs feed position/velocity data between frames for trails, physics, flocking.
- Syphon/Spout output enables integration with Resolume, VDMX, MadMapper, etc.
- NDI output for network video routing to media servers.
- Python scripting throughout the entire node network.
- OSC, MIDI, DMX, serial, WebSocket all handled natively.
- `Movie File In TOP` for HAP-encoded high-res playback.
- `Kinect CHOP` / `RealSense CHOP` / `MediaPipe CHOP` for body tracking feeding shaders.

**GLSL Shader Capabilities:**
- `GLSL TOP`: accepts up to 8 texture inputs, runs a user-written GLSL fragment shader, outputs a texture. This is the primary custom-shader node in TouchDesigner.
- Uniforms driven by any CHOP channel (audio level, LFO, sensor data, MIDI CC) — audio-reactive shader parameters with zero extra code.
- `Feedback TOP`: feeds the output of any TOP back into itself on the next frame. Standard technique for: echo trails, reaction-diffusion (Gray-Scott), cellular automata, iterated function systems.
- `GLSL Multi TOP` and `GLSL MAT` (material shaders for 3D geometry in SOPs/COMPs).
- Full access to `gl_FragCoord`, `iResolution`, custom uniform structs, `sampler2D` arrays.
- Multi-pass rendering via multiple GLSL TOPs chained in sequence.
- GPU particle simulation using GLSL TOP ping-pong: position texture → GLSL → new position texture → render.

**Signature Visual Effects Produced in Practice:**
- GPU particle systems with vertex displacement (millions of points, GLSL-driven per-particle physics steered by curl noise or audio FFT).
- Audio-reactive feedback trails: fractal-like echo loops that bloom with bass kicks, decay between beats.
- Metaballs / SDF isosurface raymarching rendered entirely in GLSL TOP.
- Displacement-mapped geometry responding to FFT frequency bands in real-time.
- Real-time fluid simulation via texture ping-pong (Navier-Stokes in GLSL: pressure, divergence, advection passes).
- Point cloud rendering from depth cameras (Kinect/RealSense), styled with custom GLSL materials per-point.
- Compositing: multiple video sources + generative layers composited in a GLSL TOP chain with custom blend math.

**2025 New Features (Experimental builds 2025.30060+):**
- **Point Operators (POPs):** New GPU-accelerated geometry operator family. Millions of animated points/particles, vector fields, fluid simulations — all GPU-native with no CPU bottleneck. Minimum 4 GB VRAM required, 8 GB recommended.
- **NVIDIA RTX Video TOP:** AI-powered super-resolution (DLSS-class) and HDR conversion via the RTX Video SDK — upgrades low-res sources inside the TD pipeline.
- **Nvidia Denoise TOP / Nvidia Upscaler TOP:** Updated to support Blackwell (RTX 50-series) GPUs.
- **ST2110 support:** Broadcast-grade uncompressed video transport for live production integration.
- **Python expansion:** Extended scripting API across more operator types.

**Educational Resources:**
- The Node Institute (thenodeinstitute.org) — semester-format GLSL courses for TD (SS25 cohort active).
- Interactive & Immersive HQ (interactiveimmersive.io) — free tutorials, GLSL terms reference.
- GitHub: `interactiveimmersivehq/Introduction-to-touchdesigner` — free open GLSL curriculum with full shader examples.
- Bruno Simon's Three.js patterns cross-apply to TD's GLSL TOP (same GLSL ES spec).

---

#### 2. Resolume Avenue / Arena + Wire

**Category:** VJ performance software + media server (Arena adds projection mapping)  
**Platform:** Windows, macOS  
**License:** Commercial  
**Website:** https://resolume.com  

**Key Strengths:**
- "Plug and play" paradigm: clip-based performance on a deck/layer matrix. Instant BPM sync. Effect chain per layer or globally. This is the most accessible professional VJ tool.
- The industry standard for Windows-based VJing. The majority of professional festival VJ booths have Resolume installed.
- 100+ built-in effects and sources (generators). All effects are plugin-based — third-party `.dll`/`.dylib` FFX plugins (Freeframe) can be dropped in.
- Arena adds: multi-projector edge blending, advanced projection mapping onto arbitrary surfaces, DMX output for syncing with lighting rigs.
- NDI, Syphon, Spout input/output for integrating generative content from TouchDesigner, VDMX, or Processing.
- BPM sync on virtually every numeric parameter.
- Current version: Arena/Avenue 7.22.3 (January 2025).

**Shader Capabilities (Wire + ISF):**
- **Resolume Wire:** Node-based visual programming environment specifically for building ISF shaders and custom effects that appear in Avenue/Arena as native plugins.
- Wire exposes ISF parameters as knobs directly in the Resolume mixer — MIDI-mappable, BPM-syncable.
- Community-contributed ISF shaders numbered in the hundreds, freely available at `editor.isf.video` — import directly into Resolume via Wire.
- Shadertoy shaders can be ported into Wire's ISF node with minor adaptations (`iTime` → `TIME`, coordinate normalization, etc.).
- Audio reactivity: Resolume's own BPM/level analyzer outputs can be piped as uniforms into ISF shaders via Wire.
- OpenGL C++ programming: custom FFX/Freeframe plugins for native performance without ISF overhead.

**Signature Visual Effects Built-In:**
- Clip-based kaleidoscope, mirror, tile effects.
- Feedback delay / echo trails (built-in Feedback source + layer routing).
- Real-time video color grading: hue rotate, levels, curves, chromatic aberration, RGB shift.
- Glitch and pixel-scramble effects (built-in + community ISF shaders).
- BPM-synced strobe, flash, beat-reactive masks, stutter effects.
- Warp/fisheye/pinch distortion on video in time with music.
- Bloom, glow, edge detection, convolution effects.

**Arena Specific:**
- Projection mapping onto complex surfaces (buildings, stage sets, LED panels, 3D objects).
- Pixel mapping: route video regions to DMX LED fixtures via Art-Net/sACN.
- Multiple output warping and blending for multi-projector setups.
- Canvas up to massive resolutions for LED wall driving.

**Ecosystem / Community:**
- Large commercial plugin ecosystem — VJ market plugins available from individual developers.
- VJ UNION forum (vjun.io): extensive Wire/ISF threads, technique sharing.
- "50 Shades of ISF" series by BenNoH: detailed ISF technique documentation for Resolume.
- "Arena Plugins Series 2024" by BennoH on VJ Union: production-grade effect building.

---

#### 3. VDMX (Vidvox)

**Category:** Modular VJ software  
**Platform:** macOS only  
**License:** Commercial (VDMX5 current; VDMX6 / VDMX6 Plus announced)  
**Website:** https://vidvox.net / https://vdmx.vidvox.net  

**Key Strengths:**
- Modular architecture: the user assembles their own interface from "plugins" (Layer, FX Chain, Video Source, Audio Analyzer, etc.). No fixed deck layout — a seasoned VJ can build an interface tuned exactly to their workflow.
- Deep ISF integration: native ISF renderer. ISF was originally designed for and by VDMX/Vidvox. Every ISF shader is a first-class VDMX object.
- Supports: Quartz Composer (legacy), CoreImage filters, Vuo compositions, TouchDesigner compositions (VDMX6 Plus).
- HAP-encoded video playback for hardware-accelerated high-resolution content.
- Powerful audio analysis: volume, FFT with per-band routing, beat detection, LFO syncable to detected BPM.
- OSC, MIDI, DMX, HID device support.
- Syphon in/out: receive from TouchDesigner, send to MadMapper.
- Can record output to disk as ProRes/HAP for content archiving.

**Shader Capabilities:**
- Native ISF renderer: any GLSL shader written to ISF spec runs directly in VDMX.
- ISF persistent buffers: multi-pass feedback shaders (reaction-diffusion, Game of Life, fluid sim) work natively — the ISF spec handles the FBO management automatically.
- ISF floating-point buffers for HDR intermediate computations.
- Audio FFT data injected into ISF uniforms via the Audio Analysis plugin — each frequency band mappable to a specific shader parameter.
- CoreImage filter chain for GPU-accelerated image processing without GLSL (for artists who don't want to write shaders).

**Signature Visual Effects:**
- Audio-reactive ISF generators: Lissajous curves, oscilloscope waveforms, spectrum analyzers, frequency-band-driven geometry.
- Chroma keying and luminance masking for multi-layer compositing.
- Perspective warp correction for projection on angled surfaces.
- 20+ blend modes GPU-accelerated across layers.
- ISF-driven generative textures: Truchet tiling, Voronoi patterns, noise fields, plasma.

**VDMX6 Plus (2024–2025):**
- New tier that allows running TouchDesigner compositions inside VDMX directly — combines TD's generative power with VDMX's performance mixing workflow. Significant workflow innovation.

---

#### 4. MadMapper

**Category:** Projection mapping + LED + DMX + laser show software  
**Platform:** macOS, Windows  
**License:** Commercial  
**Website:** https://madmapper.com  

**Key Strengths:**
- The reference tool for projection mapping onto 3D objects, buildings, stage sets.
- LED strip and matrix pixel mapping: routes video regions to pixel arrays via DMX/Art-Net.
- Laser show control integration.
- Syphon/Spout input: generative content from TouchDesigner, VDMX, Processing, etc. fed directly into the MadMapper canvas.
- Up to 16K high-resolution output canvas, driving multiple synchronized outputs.
- MadMapper Shader Editor: built-in GLSL shader editor for writing generative content that runs as a source inside MadMapper.
- "Instant generative effects": library of shader-based generators and effects — no 3D modeling needed.
- OSC, MIDI, Ableton Link sync.

**Shader Capabilities:**
- MadMapper Shader Editor: GLSL fragment shaders with ISF-like metadata for parameter exposure.
- Shaders can be shared with the MadMapper community directly.
- Standard inputs: `TIME`, `RENDERSIZE`, audio amplitude, MIDI CC values.
- Interactive AR projections: sensor/camera data drives projection content in real-time so projection responds to physical space.
- Typical high-end workflow: generate content in TouchDesigner or Resolume → Syphon into MadMapper → precisely map onto physical surfaces.

**Signature Visual Effects:**
- 3D surface mapping: turning irregular objects into video canvases.
- Building projection mapping with per-face content routing and edge masking.
- LED matrix animation with video-driven pixel effects (strips, panels, wearables).
- Real-time AR overlay: sensors/cameras drive which projection plays, creating interactive environments.

---

#### 5. Notch

**Category:** Real-time 3D generative graphics tool + "Notch Blocks" deployment format  
**Platform:** Windows  
**License:** Commercial (tool license + per-show Block deployment fee)  
**Website:** https://notch.one  

**Key Strengths:**
- The de facto standard for high-end, stadium-scale real-time generative content in live concerts (as of 2023–2025).
- "Notch Blocks" are compiled, self-contained content units that run inside media servers (Disguise, Green Hippo, SMODE, Creative Edge Roe, etc.) without requiring the Notch authoring tool at runtime. Industry standard interop format for live content.
- Node-based scene graph (similar to TD but with a 3D-first focus and cleaner artist-friendly UI).
- Four physically based GPU renderers ("NURA") — users switch between them in real time without changing scene, lighting, or materials.
- Real-time performer tracking: tracks moving bodies/faces via camera, drives visual effects from skeleton/face data.
- AI background removal and GPU green screen keying — production-grade, low latency.
- Motion analysis, optical flow, feature tracking (face, body, hands) — all built-in, no separate ML pipeline.
- Any node parameter can be changed live over network/OSC during a show — LD or VJ can tweak from front-of-house.
- Natively integrates with the Disguise media server ecosystem — the GX 3+ is the primary Notch Block runtime platform.

**Shader / Rendering Capabilities:**
- Particle system with per-particle material shaders, forces, colliders, emitter shapes.
- Volumetric rendering: ray-marched volumes, clouds, fog, subsurface scattering.
- Post-processing chain: bloom, glow, depth of field, chromatic aberration, motion blur, film grain, vignette, color grade LUT.
- PBR materials: physically based rendering with real-time Lumen-style global illumination (in Notch 1.0).
- Video processing: real-time keying, warp, optical flow displacement, color grading on live video.
- HLSL custom material shaders on geometry for artists who need shader-level control.

**Notable Productions (2023–2025):**
- **Phish at Sphere Las Vegas (2024, 2026):** Moment Factory drove real-time Nanite/Lumen (via Unreal Engine 5) scenes; lighting board sliders controlled on-screen visual parameters via DMX plugin.
- **Anyma "The End of Genesys" at Sphere (NYE 2024, 130,000 tickets across residency):** Unreal Engine 5 with Nanite + Lumen + AI generative tools. AI-driven procedural animations reacted dynamically to music. 16K LED wrap-around display fully utilized.
- **Taylor Swift "Eras Tour":** Disguise GX3 + Notch Blocks — massive LED wall arrays.
- **Coldplay "Music of the Spheres" World Tour:** Disguise pipeline, LED wristbands (Xylobands) synced to stage visuals.
- **Samsung Galaxy AI Immersive Music Festival (Ho Chi Minh City, Summer 2024):** Mobile phone facial motion capture + Notch 1.0 effects applied in real-time to performer faces.
- **Eurovision 2024, VMAs 2024, Coachella:** All Disguise media server backbone.

**Ecosystem:**
- Disguise GX 3+ is the primary hardware runtime. GX 3 is "the go-to media server for the world's biggest live events of the past three years."
- Green Hippo, SMODE also deploy Notch Blocks.
- CDM (Create Digital Music) covered Notch extensively: "Notch is the best option to create real-time effects."

---

#### 6. vvvv gamma + VL.Fuse

**Category:** Visual live-programming environment for .NET  
**Platform:** Windows  
**License:** Free for non-commercial; commercial license for deployment  
**Website:** https://vvvv.org  

**Key Strengths:**
- Visual dataflow + functional + OOP hybrid language (VL) — patches are compiled programs, not interpreted.
- "Always runtime" model: no compile/restart cycle. Patch changes apply instantly — critical for live performance where stopping a show to recompile is unacceptable.
- Runs on the .NET runtime; performance equivalent to compiled C#.
- 2D rendering fully GPU-accelerated via Stride 3D engine.
- Extensive hardware integration: DMX, OSC, MIDI, serial, WebSocket, video capture, depth cameras.
- Heavy use in European interactive arts/tech scene (NODE festival, Berlin; Resonate, Belgrade).

**VL.Fuse Library (the GPU powerhouse):**
- GitHub: https://github.com/TheFuseLab/VL.Fuse — open-source, community-supported.
- Provides visual GPU programming without writing shader code: patch logic in the vvvv graph, FUSE compiles it to SDSL (Stride's OOP shader language, a superset of HLSL).
- Core modules: Distance Fields & Raymarching, Particles, Procedural Geometry, Textures and Materials, GPGPU compute shaders.
- Can port Shadertoy shaders into FUSE using the ShaderToy-import node.
- Compute shaders without writing HLSL — patch a GPGPU workflow visually.
- TextureFX: large built-in collection of GPU texture effects (blur, distort, color grade, blend, warp).

**SDSL Shader Language:**
- OOP with multiple inheritance — write short, composable shader code.
- Shader hot-reload: edit SDSL file, instantly see the change in the running sketch.
- Full GPU instancing for millions of objects.
- SDF raymarching available as FUSE nodes without any shader code.

**Signature Visual Effects:**
- SDF-based raymarched scenes entirely within the vvvv graph — no external shader files needed.
- Large-scale GPU particle systems with GPGPU physics (millions of particles).
- Procedural geometry that morphs and reacts in real-time.
- Custom PBR materials on 3D geometry, driven by audio analysis.
- NODE Festival Berlin regularly features vvvv/FUSE-based performances.

---

#### 7. openFrameworks

**Category:** Open-source C++ creative coding toolkit  
**Platform:** Windows, macOS, Linux, iOS, Android  
**License:** MIT (open source)  
**Website:** https://openframeworks.cc  

**Key Strengths:**
- C++ performance: lowest-level GPU access of any tool listed. Zero overhead between shader and hardware.
- `ofShader` class wraps GLSL vertex + fragment + geometry + compute shaders with hot-reload.
- `ofxFX` addon by Patricio Gonzalez Vivo: GPU effects toolkit with GLSL convolutions, blurs, bloom, feedback, ping-pong buffers. https://github.com/patriciogonzalezvivo/ofxFX
- `ofxPostProcessing`: standard post-processing effects chain.
- Wide addon ecosystem: ofxAddons.com (700+ community addons).
- Handles: video, audio (with FFT via `ofxAudioAnalyzer`), OSC, MIDI, serial, webcam, depth cameras, OpenCV integration.

**Notable Artists:**
- **Zachary Lieberman:** hand-drawn-style typography + webcam interaction with custom oF + GLSL pipeline.
- **Memo Akten:** physics-based cloth + fluid dynamics in real-time with GLSL. Landmark installation "All Is Full Of Love" used oF.
- **Kyle McDonald:** face-swap, feature tracking, generative text — extensive oF use.

**Shader Capabilities:**
- Direct GLSL: write vertex, fragment, geometry, compute shaders with full OpenGL API access.
- `ofxFX` effects: Gaussian blur, Dilate, Erode, Light Leak, God Rays, Bloom, Kuwahara painterly filter, feedback accumulation, pixel-sort.
- Ping-pong FBO pattern: `ofFbo` ping-pong for reaction-diffusion, fluid sim, CA — structurally identical to TD's Feedback TOP or Three.js WebGLRenderTarget ping-pong.
- Multi-pass rendering via FBO chaining.
- GPU particle systems with transform feedback or compute shaders.

**Live Performance Workflow:**
- Build custom performance software in oF → output via Syphon → receive in Resolume/VDMX for mixing.
- Artists documented live set using "many patterns, complex generative shapes, and minimal use of color, performing live at notable New York City venues" (ecency.com article on oF live performance).

---

#### 8. Processing / p5.js

**Category:** Creative coding language (Processing = Java; p5.js = JavaScript/WebGL)  
**Platform:** Cross-platform  
**License:** Open source (LGPL)  
**Website:** https://processing.org / https://p5js.org  

**Key Strengths:**
- Lowest entry point for creative coding. Enormous educational community.
- Processing: `PShader` class for GLSL, `PGraphics` for FBO-based multi-pass rendering.
- p5.js + WebGL: runs in-browser, GLSL shaders via `createShader()`, composited with 2D canvas drawing.
- OSC input via `oscP5` library for audio-reactive work.
- Used for music visualization, data-driven visuals, projection mapping (Syphon output).

**Shader Capabilities:**
- GLSL fragment + vertex shaders via `PShader` (Processing) or `createShader` (p5.js/WebGL).
- No built-in multi-pass or persistent buffers — requires manual `PGraphics` chaining in Processing; `createGraphics(w, h, WEBGL)` for p5.js.
- p5.js GLSL shaders shared on OpenProcessing.org — large community shader library.

**Limitations for Live Performance:**
- Not designed as a performance tool: no clip matrix, no BPM sync, no built-in MIDI mapping GUI.
- Typically used as a content generation tool — output Syphon → Resolume/MadMapper for live deployment.
- For SpaceGen (Three.js/WebGL/Electron): Processing patterns are architecturally similar. p5.js shaders port directly to Three.js `ShaderMaterial` with trivial changes.

---

#### 9. Millumin

**Category:** AV show creation and media server  
**Platform:** macOS (Apple Silicon optimized)  
**License:** Commercial (V5 current — free upgrade from 2024 V4 licenses)  
**Website:** https://millumin.com  

**Key Strengths:**
- Designed for theater, dance, video mapping, interactive installation — timeline-based show playback combined with interactive/generative capabilities.
- ISF effects: native ISF renderer, uses built-in ISF library, supports community ISF shader installation.
- HAP + ProRes 422 4K multi-layer playback tested at high performance on M3 Max MacBook Pro (High Resolution Engineering benchmark, January 2024).
- Syphon in/out for integration with TouchDesigner, VDMX, etc.
- Bluefish444 hardware I/O integration for professional broadcast-grade output.
- People-tracking effects on Apple Silicon (ML-based body detection via Core ML).
- After Effects plugin for precise slice-based content preparation.
- Addons: DMX, OSC, MIDI.

**Shader Capabilities:**
- ISF as the primary shader runtime: color effects, distortion, stylize, tile, blend modes all ISF-based.
- ISF persistent buffers supported — multi-pass feedback shaders work natively.
- QR code / barcode tracking and people-tracking effects (Apple Silicon ML hardware acceleration).

---

#### 10. Modul8 (GarageCUBE)

**Category:** Live video mixing  
**Platform:** macOS  
**License:** Commercial  
**Status:** Largely superseded by VDMX and Resolume in professional contexts, but still used in established workflows.

**Key Strengths:**
- Pioneered the "layer-based live VJ mixer" paradigm on macOS (2003–2016 peak era).
- Quartz Composer plugin support.
- Basic video mixing, effects, audio reactivity.
- Limited compared to VDMX/Resolume in current feature set; not receiving major updates.
- Still relevant to mention for legacy compatibility when working with VJ artists who learned on it.

---

#### 11. Hydra (Olivia Jack / TOPLAP)

**Category:** Browser-based live-coding video synthesizer  
**Platform:** Web browser (WebGL)  
**License:** Open source (MIT)  
**Website:** https://hydra.ojack.xyz  

**Key Strengths:**
- Inspired by analog modular synthesizers: "patch cable" metaphor implemented as JavaScript function chains. Each function compiles to a GLSL fragment shader — the user never writes GLSL directly.
- Four output buffers (o0–o3) that can sample each other → feedback loops, recursive compositing, hall-of-mirrors effects natively.
- Network collaboration via WebRTC: multiple users patch the same canvas in real-time (Algorave/TOPLAP performance use case). Flok (flok.cc) extends this with shared live-coding sessions.
- `setFunction()` API: add custom GLSL snippets to the Hydra function set — bridge between ease-of-use and full shader control.
- Audio input via Web Audio API.
- Runs entirely in-browser, zero installation.

**Signature Visual Effects:**
- Feedback hall-of-mirrors with modulated rotation and zoom: `src(o0).scale(0.99).rotate(0.01).blend(o0).out()`
- Voronoi noise fields with per-cell color oscillation.
- Kaleidoscope from live camera feed: `src(s0).kaleid(6).out()`
- Waveform-based luma keying between video sources.
- Oscilloscope-like patterns: `osc(40, 0.1, 1.5).rotate(1.57).out()`
- Used extensively at Algorave and TOPLAP live-coding performances globally.

**Engineering relevance for SpaceGen:** Hydra's source code is open — the WebGL framebuffer pipeline it uses is architecturally similar to how SpaceGen would implement ping-pong feedback. The `setFunction()` mechanism is a good model for a user-facing shader extension API.

---

#### 12. KodeLife (hexler)

**Category:** Real-time GPU shader editor / live-coding performance tool  
**Platform:** macOS, Windows, Linux, Raspberry Pi, iOS, Android  
**License:** Commercial (affordable, ~$20)  
**Website:** https://hexler.net/kodelife  

**Key Strengths:**
- Minimal, focused GLSL/Metal/HLSL live editor. Code is evaluated as you type — no compile button. Shader changes appear on screen within milliseconds.
- Used for live shader performance and rapid prototyping of effects before integrating into production pipelines.
- Inputs: `time`, `resolution`, `mouse`, custom textures, audio FFT (microphone/line-in as 512×1 texture), MIDI CC values as uniforms.
- `backbuffer` uniform: previous frame texture built in — feedback effects without managing FBOs.
- Supports fragment + vertex + geometry shaders.
- Multi-pass rendering via Pass A/B configuration.
- Syphon/Spout output to feed other VJ software.
- Available on mobile platforms — live-coding from a phone/tablet is documented in the TOPLAP community.

**Keijiro Takahashi's ShaderSketches collection:**
- GitHub: https://github.com/keijiro/ShaderSketches
- 50+ KodeLife GLSL sketches covering: wave interference patterns, feedback, cellular noise, Lissajous figures, Truchet tiling, SDF-based 2D shapes, gradient color cycling, domain warping experiments.
- Widely referenced as a learning resource in the live-coding community.

---

#### 13. Disguise (d3) Media Server

**Category:** Professional media server hardware + software platform  
**Platform:** Proprietary hardware (GX 3, GX 3+, VX4, rx II, solo, etc.)  
**License:** Commercial hardware purchase + software subscription  
**Website:** https://disguise.one  

**Key Strengths:**
- The dominant media server platform for stadium-scale touring productions.
- GX 3 / GX 3+ runs Notch Blocks natively with top-tier GPU hardware (multiple high-end GPUs per unit).
- Productions: Taylor Swift, Coldplay, Eurovision, Coachella, VMAs (2023–2025) — all Disguise backbone.
- Real-time Notch Block playback at any scale without added live video latency.
- AI background removal, facial tracking, optical flow — built into the Disguise runtime.
- Canvas up to 32 HD layers or ultra-high-resolution single canvas (16K+).
- Hardware specs (GX 3+): designed for "next-generation Notch blocks with Notch 1.0 and GX 3+" (disguise.one blog).

**Content tools supported:** Notch Blocks, Unreal Engine (via nDisplay integration), custom video clips, generative sources.

---

#### 14. Unreal Engine (Epic Games) — Live Events Use

**Category:** Game engine deployed as real-time media server  
**Platform:** Windows (primary deployment), macOS (editor)  
**License:** Free (royalty terms apply to commercial products; live events typically license separately)  
**Website:** https://unrealengine.com/uses/broadcast-live-events  

**Key Strengths for Live Production:**
- **Nanite virtualized geometry (UE5):** film-quality geometric detail at real-time frame rates. Critical for the Sphere's 16K display fidelity.
- **Lumen global illumination:** dynamic GI — when the LED wall IS the stage lighting, Lumen produces physically correct response to screen color on performer and stage.
- **DMX plugin:** lighting desk sliders → UE5 material parameters. LD controls generative content from a conventional lighting console, enabling non-technical show operators to drive real-time visuals.
- **nDisplay:** multi-GPU, multi-machine rendering cluster for massive LED wall and projection setups.
- **Media framework:** capture UE output as video texture, send via NDI, SDI, HDMI.

**Notable Productions:**
- **Phish at Sphere (2024, 2026):** Moment Factory. DMX from lighting desk drove real-time Nanite/Lumen parameters — lighting board sliders = on-screen visual changes.
- **Anyma "The End of Genesys" at Sphere (NYE 2024):** UE5 with Nanite + AI generative tools; 16K wrap-around.
- **Coachella/Flume AR performance:** UE5 as compositing backbone for live AR broadcast.
- **deadmau5:** live-rendered UE visuals on LED wall.
- **ABBA Voyage:** digital avatars rendered real-time in UE (proprietary pipeline).

---

### Companion / Niche Tools

| Tool | Role | URL |
|------|------|-----|
| **Synesthesia** | Audio-reactive shader runtime for live performance (SSF format — provides `syn_BPMSin`, `syn_BassTime`, `syn_HighHits`, `syn_RandomOnBeat` as shader uniforms) | https://synesthesia.live |
| **Smode** | French media server, supports Notch Blocks and ISF; used in theater/installation | https://smode.io |
| **Green Hippo** | Media server with Notch Block support; used in concert touring | https://green-hippo.com |
| **Hippotizer** | Media server by Green Hippo; large festival + corporate events install base | https://green-hippo.com |
| **Millumin** | Theater/dance AV server; ISF native; Apple Silicon optimized | https://millumin.com |
| **CoGe** | Mac VJ app; ISF native; less active development | https://imimot.com/coges |
| **Videosync** | Ableton Live plugin for generative video; ISF support | https://videosync.fi |
| **GrandVJ** | ArKaos media server; used in clubs/festivals | https://www.arkaos.com |

---

## Top Visual Effects for Live Concerts / Clubs / Festivals (2022–2025)

Based on documented productions, VJ community forums, shader libraries, and industry reporting.

---

### Tier 1: Universally Present — Every Major Production

**1. Audio-Reactive Particle Systems**
- Millions of GPU particles steered by curl noise, with velocity impulses triggered by FFT frequency bands.
- Bass = expansion force; mid = turbulence amplitude; high = spark burst frequency.
- Visual forms: speaker-membrane pulse, star-field collapse, fluid trail lines, galaxy spiral.
- Implementation: GPGPU position/velocity texture ping-pong. TD Feedback TOP + GLSL TOP. vvvv FUSE particle module. Notch built-in particle system.
- Seen in: virtually every electronic music concert with LED walls (2022–2025).

**2. Feedback / Echo Trails**
- Read last frame's texture, apply slight zoom (0.995×), rotation (0.001 rad), and color drift (×0.97 per channel), blend with current frame.
- Over time: hypnotic spiral echo effect that gives visual "reverb" to any source.
- Modulation: zoom speed synced to BPM, rotation driven by audio phase, hue shift per frame.
- Extremely popular in techno, industrial, and electronic ambient club contexts.
- All VJ platforms support this natively (TD Feedback TOP, ISF persistent buffers, Resolume Feedback source, Hydra `src(o0).scale(0.99).out()`).

**3. Chromatic Aberration / RGB Shift**
- Offset R, G, B texture samples independently in UV space: `col.r = tex(uv + vec2(0.003, 0)).r` etc.
- Used as a "distress" post-process — adds analog-camera-failure texture to any source.
- Intensity driven by snare/kick hit (envelope: fast attack, exponential decay → 0).
- Found in: Resolume built-in FX, ISF shaders, TouchDesigner post-processing, Notch post chain.

**4. Kaleidoscope / Radial Mirror**
- Convert to polar `(r, θ)`, take `θ = mod(θ, 2π/N)`, mirror at segment midpoint, sample input texture.
- N-fold symmetry applied to any source (particle system, camera feed, generative noise).
- Popular in psytrance, trance, festival daytime stages. Produces mandala geometry.
- All platforms support it natively as an effect.

**5. Glitch / Datamosh**
- Datamoshing: remove I-frames from video compression, causing motion vectors to "bleed" between clips.
- GLSL implementation: use previous frame optical flow vector to displace pixels — pixels from one scene applied to another.
- Pixel sort: sort pixels per column by brightness above a threshold — crystalline streaks effect.
- RGB band shift: random horizontal shifts per scan-line `(floor(uv.y * 30) * rand → UV.x offset)`.
- Extremely popular accent effect for accent moments (snare hits, drops, transitions).

---

### Tier 2: Common in Club / Festival Contexts

**6. SDF Raymarching — Geometric / Fractal Scenes**
- Entire 3D scene from mathematical distance functions in one fragment shader.
- Key technique: Inigo Quilez SDF primitives — sphere, box, torus, capsule — combined with `smin()` for smooth blending.
- Domain repetition (`opRep()`) for infinite tunnels. `opTwist()` for deformed structures.
- Displaced SDF with fBm noise → organic terrain-like forms.
- Ambient occlusion from SDF gradient: iterate `clamp(f(p + n*0.01) / 0.01, 0, 1)`.
- Popular in: festival main stage (high-fidelity look), visual artist sets where video loops look too "pre-rendered."

**7. Domain Warping with fBm**
- `fBm(p + fBm(p))` (and nested iterations) produces swirling organic forms: nebulae, lava, smoke, alien weather.
- Animating `p += iTime * 0.1` gives seamless drifting motion — perfect for slow-burn ambient stages.
- Beat-reactive: scale warp amplitude with bass amplitude for sudden turbulence on drop.
- Reference: iquilezles.org/articles/warp (the canonical implementation).

**8. Voronoi / Cellular Patterns**
- Cracked glass, cell, crystal facet, scale textures from nearest-neighbor distance field.
- Animated by moving seed points in time (Lissajous, audio-driven trajectories).
- F2-F1 border highlighting → neon grid on organic background.
- 3D Voronoi carves space into organic chambers for SDF scenes.
- Popular in: tech/industrial aesthetics, futuristic stage design, biomechanical visuals.

**9. Volumetric / Raymarched Clouds and Fog**
- Ray-march through a volume defined by 3D fBm density function, accumulating density and light transmittance.
- "Protean Clouds" (Shadertoy: 3l23Rh) — one of the most-viewed shaders on Shadertoy — runs at 1080p real-time, fully procedural 3D animated cloud volume.
- "Clouds" (Shadertoy: XslGRr) by Inigo Quilez — canonical reference.
- Used for: atmospheric backgrounds, smoke effects behind performers, fog layers in LED wall content.

**10. Reaction-Diffusion (Gray-Scott Model)**
- Two chemical species A/B diffuse and react: `dA/dt = Du·∇²A - A·B² + f·(1-A)`, `dB/dt = Dv·∇²B + A·B² - (f+k)·B`.
- Produces Turing patterns: spots, labyrinths, stripes, waves — all slowly evolving.
- Implementation: ping-pong GLSL. Laplacian via 3×3 texture sampling kernel.
- Parameters (F, K) can be gradually modulated by audio for dynamic pattern shifts.
- Popular in: club visuals (slow-evolving organic patterns as background), generative art installations.

---

### Tier 3: Specialist / High-Impact Moments

**11. Warp-Speed Tunnel / Zoom Tunnel**
- UV → polar: `uv.x = atan2(y,x)/(2π)`, `uv.y = 0.5/length(xy) + time*speed`.
- Produces infinite zoom through a tube. Star-field variant: point-sample noise for stars.
- Extreme popularity for electronic music drops and breakdowns.
- Variant: Apollonian gasket tunnel (recursive circle packing in SDF) for fractal tunnel.

**12. Lens Flare / God Rays**
- Anamorphic lens flare: bright horizontal streak from light source with diffraction rings.
- God rays (crepuscular rays): radial blur from bright source through occluder (march ray from pixel toward light, accumulate sample-based attenuation).
- Used as accent effects on light sources in 3D scenes, high-end production feel.

**13. Fluid Simulation**
- Navier-Stokes on GPU: velocity + pressure + divergence solved in 3–4 GLSL passes per frame.
- Produces realistic fluid interaction: stir with audio FFT "force injection."
- Computationally expensive — typically run at half resolution with upscaling.
- Used in: immersive art installations, high-budget festival main stages.

**14. AI-Driven Real-Time Visual Effects (2023–2025 emerging)**
- Live Stable Diffusion: feed live generative content or camera into StreamDiffusion (ComfyUI), get AI-painted output back. Latency 3–10 fps on RTX 3090, improving rapidly.
- Performer face tracking via MediaPipe / ARKit → drive Notch/TD shader parameters from facial blendshapes.
- Optical flow from camera feed → flow field driving particle advection or texture displacement.
- Anyma Sphere: ML algorithms for procedural animations reactive to music in real-time (UE5 + custom ML).
- NVIDIA RTX Neural Shaders (RTX 50-series hardware): tiny MLPs compiled into GPU shaders for neural texture compression and neural material appearance — not yet consumer-accessible for WebGL, but arriving 2025–2026.

---

## Shader Libraries — Detailed Survey

---

### 1. ISF (Interactive Shader Format)

**Spec version:** 2.0  
**Maintained by:** Vidvox  
**Primary website:** https://isf.video / https://editor.isf.video  
**GitHub (official):** https://github.com/Vidvox/ISF-Files  
**GitHub (community):** https://github.com/bareimage/ISF  

**Technical specification:**
- A GLSL fragment shader wrapped in a JSON metadata header specifying:
  - `INPUTS`: typed parameters — `float`, `color`, `image`, `audio`, `audioFFT`, `boolean`, `long` (enum), `point2D`, `event`.
  - `PASSES`: multi-pass rendering order with named intermediate textures.
  - `PERSISTENT_BUFFERS`: named FBOs that persist between frames (enables feedback, cellular automata, fluid sim without external FBO management).
  - `IMPORTED`: external images/textures to load.

**Categories and scale:**
- **Generators** (no input image required): plasma, Lissajous, Truchet, Voronoi, noise fields, waveforms, oscilloscopes, geometric patterns. ~100+ in official repo.
- **Effects/Filters** (process an input image): blur, sharpen, color grade, chromatic aberration, glitch, pixel sort, warp, mirror, kaleidoscope, edge detect, emboss. ~100+ in official repo.
- **Transitions** (blend between two images): cross-dissolve, wipe, burn, glitch transition, luma-matte transition, ripple. ~50+ in official repo.
- Total: 300+ official shaders in `Vidvox/ISF-Files` repo; hundreds more in community at `editor.isf.video`.

**Supported applications:**
VDMX, Resolume (via Wire), Millumin, MadMapper, CoGe, Smode, Videosync, Max/MSP (`jit.gl.isf`), Apple Motion (ISF for Motion plugin — 200+ shaders), TouchDesigner (via custom ISF parser/importer).

**Engineering note for SpaceGen (Three.js porting):**
ISF shaders are trivially portable to Three.js `ShaderMaterial`:
1. Strip the JSON header block.
2. Map `isf_FragNormCoord` → `vUv` (from a vertex shader that passes `uv` as varying).
3. Map `TIME` → `uniform float uTime`.
4. Map `RENDERSIZE` → `uniform vec2 uResolution`.
5. Map `IMG_THIS_PIXEL(inputImage)` → `texture2D(uTexture, vUv)`.
6. Map `IMG_NORM_PIXEL(inputImage, coord)` → `texture2D(uTexture, coord)`.
7. For persistent buffers: implement as ping-pong `WebGLRenderTarget` pairs.
The core GLSL runs without modification in all cases (GLSL ES 1.0/3.0 compatible).

---

### 2. Shadertoy

**Website:** https://www.shadertoy.com  
**Scale:** 80,000+ public shaders as of 2024.  
**Technology:** WebGL (GLSL ES 3.0). Runs in browser.  
**Revision 2024 Shader Showdown:** live shader coding competition at the demoparty — participants write GLSL from scratch in 25 minutes. YouTube: "Revision 2024 - Shader Showdown Qualifications."

**Standard uniforms injected:**
```glsl
uniform vec3      iResolution;           // viewport res (width, height, pixel ratio)
uniform float     iTime;                  // shader playback time (seconds)
uniform float     iTimeDelta;             // render time (seconds)
uniform int       iFrame;                 // frame number
uniform float     iChannelTime[4];        // channel playback time
uniform vec3      iChannelResolution[4];  // channel resolution (pixels)
uniform vec4      iMouse;                 // mouse: xy=current, zw=click
uniform samplerXX iChannel0..3;          // input channels (image/buffer/audio)
uniform vec4      iDate;                  // year, month, day, time in seconds
uniform float     iSampleRate;            // audio sample rate
```

**Multi-pass rendering:** Buffers A/B/C/D → Image (5-pass max). Enables: physics simulations, fluid dynamics, feedback loops, spatial hashing for collision detection.

**Audio input:** `iChannel` set to microphone — FFT and waveform data available as a 512×2 texture. Row 0 = FFT magnitudes; Row 1 = waveform samples.

**Most technically influential shaders (engineering reference list):**

| Shader | Author | ID | Technique |
|--------|--------|-----|-----------|
| Raymarching Primitives | Inigo Quilez | Xds3zN | Complete SDF primitive reference |
| Elevated | Inigo Quilez | MdX3Rr | Terrain raymarching + fBm heightmap |
| Clouds | Inigo Quilez | XslGRr | Volumetric raymarching |
| Protean clouds | Nimitz | 3l23Rh | Fast 3D animated cloud volume, 1080p realtime |
| Seascape | Alexander Alekseev (TDM) | Ms2SD1 | Ocean surface + ray-march depth |
| Very fast procedural ocean | afl_ext | MdXyzX | Optimized wave accumulation, ported to Unity/Three.js |
| Snail | Inigo Quilez | ld3Gz6 | Complex SDF scene, organic forms |
| Happy Jumping | Inigo Quilez | 3lsSzf | Character animation + SDF |
| Voronoi basic | Inigo Quilez | MslGD8 | Canonical Voronoi reference |
| Mandelbulb | — | — | Real-time 3D Mandelbulb fractal |
| Mandelbox | — | — | 3D Mandelbox fractal |
| Menger Sponge | — | — | SDF Menger sponge with domain repetition |
| Cosine palette demo | Inigo Quilez | ll2GD3 | Animated cosine color palette |
| Shader Art Coding Intro | kishimisu | — | SDF + palette tutorial, widely circulated |
| Fractal Land (Kali) | Kali | XsBXWt | Kali set fractal landscape |
| Reaction-diffusion (Gray-Scott) | — | lXXcz7 | Gray-Scott model |
| Domain warp fBm | — | wttXz8 | iq's domain warping formula |

**Shadertoy → Three.js porting cheatsheet:**
```glsl
// Shadertoy          → Three.js uniform
// iTime              → uniform float uTime;
// iResolution.xy     → uniform vec2 uResolution;
// iMouse.xy          → uniform vec2 uMouse;
// fragCoord          → gl_FragCoord (same in GLSL ES)
// mainImage(out,in)  → void main() { gl_FragColor = ...; }
// iChannel0          → uniform sampler2D uTexture0;
// Buffer A           → WebGLRenderTarget ping-pong
// Audio iChannel     → DataTexture from AnalyserNode getByteFrequencyData()
```

---

### 3. GLSL Sandbox

**Website:** https://glslsandbox.com  
**Status:** Maintenance mode as of 2024 — gallery browsable and forkable, no new submissions.

**What it is:** Minimal WebGL GLSL editor with only `uniform float time`, `uniform vec2 resolution`, `uniform vec2 mouse`. No multi-pass, no audio, no named buffers.

**Engineering value:** The gallery is a large searchable archive of pure fragment shader experiments. Many pioneering plasma, tunnel, fractal, and noise patterns originated here before Shadertoy became dominant (2011–2016 era). Good reference for minimal, self-contained shader techniques. The simplicity of the API (time + resolution only) makes these shaders the easiest to port to any target.

---

### 4. KodeLife Shader Sketches (Keijiro Takahashi)

**GitHub:** https://github.com/keijiro/ShaderSketches  
**Count:** 50+ GLSL sketches.  

**Categories covered:**
- Wave interference patterns (superimposed sine waves in UV space).
- Feedback loop experiments using the built-in `backbuffer` uniform.
- Cellular noise (Worley, animated Voronoi).
- Lissajous figures and parametric curve drawing via SDF.
- Truchet tiling (rotated quarter-circle tiles producing continuous patterns).
- SDF-based 2D shape composition.
- Gradient color cycling with cosine palettes.
- Domain warping experiments.

**KodeLife shader uniforms (relevant for SpaceGen architecture reference):**
```glsl
uniform float     time;         // seconds since launch
uniform vec2      resolution;   // viewport resolution
uniform sampler2D backbuffer;   // previous frame (free feedback)
uniform sampler2D fft;          // 512×2 audio FFT texture (row0=FFT, row1=waveform)
uniform sampler2D midi;         // MIDI CC values as texture
uniform sampler2D texture0;     // ..texture7: user-loaded images
```
The `fft` texture design is directly applicable to SpaceGen: sample `fft` at different U coordinates to get different frequency bins, use the value as an amplitude multiplier for any visual parameter.

---

### 5. LYGIA Shader Library

**Website:** https://lygia.xyz  
**GitHub:** https://github.com/patriciogonzalezvivo/lygia  
**Author:** Patricio Gonzalez Vivo (author of The Book of Shaders)  
**License:** Prosperity License (non-commercial free) + Patron License (commercial, via sponsorship)  
**Languages:** GLSL, HLSL, Metal, WGSL, CUDA — same function, multiple language outputs.  

**Structure:** granular `#include` headers. Use only what you need.

**Key modules for live VJ / generative work:**

| Module path | Contents |
|-------------|----------|
| `lygia/generative/fbm.glsl` | fBm over Value/Simplex noise, configurable octaves |
| `lygia/generative/noised.glsl` | Gradient noise with analytical derivative |
| `lygia/generative/voronoi.glsl` | Voronoi (F1, F2, F2-F1, smooth) |
| `lygia/generative/worley.glsl` | Worley cellular noise |
| `lygia/generative/curl.glsl` | Curl noise for divergence-free particle flow |
| `lygia/color/palette/cosine.glsl` | Inigo Quilez cosine palette formula |
| `lygia/color/space/rgb2hsv.glsl` | RGB ↔ HSV conversion |
| `lygia/color/space/rgb2oklab.glsl` | Perceptually uniform OKLab color space |
| `lygia/color/tonemap/aces.glsl` | ACES tone mapping (film-grade HDR → SDR) |
| `lygia/math/sdf.glsl` | SDF primitive library (sphere, box, torus, cylinder, capsule…) |
| `lygia/math/sdf2d.glsl` | 2D SDF primitives |
| `lygia/math/rotate2d.glsl` | 2D rotation matrix |
| `lygia/math/rotate3d.glsl` | 3D rotation matrix (axis/angle) |
| `lygia/filter/blur/gaussian.glsl` | Gaussian blur |
| `lygia/filter/blur/kawase.glsl` | Kawase blur (fast approximation) |
| `lygia/filter/edge.glsl` | Edge detection (Sobel) |
| `lygia/draw/circle.glsl` | Anti-aliased circle SDF |
| `lygia/draw/rect.glsl` | Anti-aliased rectangle SDF |
| `lygia/lighting/pbr.glsl` | PBR BRDF (GGX Cook-Torrance) |
| `lygia/space/kaleidoscope.glsl` | Polar mirror kaleidoscope |
| `lygia/space/ratio.glsl` | Aspect-ratio-correct UV normalization |

**Usage in Three.js / SpaceGen:**
Two options:
1. **Online resolver:** `#include "https://lygia.xyz/generative/fbm.glsl"` — lygia.xyz resolves at serve time.
2. **Local clone:** `git clone https://github.com/patriciogonzalezvivo/lygia` → add to glsl build step via `glsl-loader` or similar.

**Engineering recommendation:** LYGIA is the highest-value shader library for SpaceGen's Three.js pipeline. It eliminates re-implementing noise, SDF, color, and filter functions from scratch, and it's multi-language so the same functions can be tested in Metal (macOS) and deployed in GLSL/WGSL.

---

### 6. The Book of Shaders (Foundational Reference)

**Website:** https://thebookofshaders.com  
**Author:** Patricio Gonzalez Vivo & Jen Lowe  
**License:** Public / open educational resource  

**Engineering-critical chapters:**
- **Ch.11 — Noise:** Value noise, Gradient noise (Perlin), implementation from scratch in GLSL. Covers 2D and 3D variants.
- **Ch.12 — Cellular Noise:** Voronoi, Worley noise, F1 and F2-F1 variants.
- **Ch.13 — Fractal Brownian Motion:** fBm construction, octaves, lacunarity, gain. Domain warping.

**Companion resource — `patriciogonzalezvivo/glsl-noise` (GitHub Gist):**
The canonical GLSL noise function collection, used by thousands of VJ shaders worldwide:
- `snoise(vec2)`, `snoise(vec3)`, `snoise(vec4)` — Simplex noise.
- `cnoise(vec2)`, `cnoise(vec3)` — Classic Perlin noise.
- `pnoise(vec2, period)` — Periodic Perlin noise (seamlessly tileable).
URL: https://gist.github.com/patriciogonzalezvivo/670c22f3966e662d2f83

---

### 7. Inigo Quilez Articles (iquilezles.org)

The single most referenced technical resource for SDF, noise, and color in live shader coding. Every VJ platform's advanced users cite these.

**Essential articles for SpaceGen implementation:**

| Article | URL | What it covers |
|---------|-----|---------------|
| Raymarching Distance Functions | iquilezles.org/articles/raymarchingdf | Sphere-tracing algorithm fundamentals |
| Distance Functions (3D SDF) | iquilezles.org/articles/distfunctions | Complete SDF primitive library: 30+ shapes |
| Distance Functions (2D SDF) | iquilezles.org/articles/distfunctions2d | 2D SDF primitives for UI/overlay effects |
| Smooth Minimum | iquilezles.org/articles/smin | `smin()` for organic SDF blending |
| Domain Warping | iquilezles.org/articles/warp | fBm domain warp formula |
| Voronoise | iquilezles.org/articles/voronoise | Parametric Voronoi/Perlin blend |
| Palettes | iquilezles.org/articles/palettes | Cosine palette formula |
| Ambient Occlusion | iquilezles.org/articles/sphereao | AO from SDF — adds depth to raymarched scenes |
| Soft Shadows | iquilezles.org/articles/rmshadows | Soft shadows via SDF |
| Normal Computation | iquilezles.org/articles/normalsSDF | Analytical vs. finite-difference normals from SDF |
| Binary Search SDF | iquilezles.org/articles/binarysearchsdf | Precision refinement for SDF rendering |
| Noise Derivatives | iquilezles.org/articles/noisederivatives | Analytical derivatives for smooth noise |

**Historical impact:** Inigo Quilez's "Slisesix" (2008, 4KB intro) was the first public SDF raymarching demo to combine smooth SDF blending, domain repetition, ambient occlusion, and soft shadows in a 4KB executable. It launched the modern SDF raymarching era in the demoscene and, subsequently, Shadertoy.

---

## Emerging Trends (2024–2026)

---

### 1. AI Neural Shaders (NVIDIA RTX Neural Shaders)

**Announcement:** CES 2025  
**Hardware requirement:** GeForce RTX 50-series (Blackwell architecture, "cooperative vectors" instruction set)  
**Reference:** https://developer.nvidia.com/blog/nvidia-rtx-neural-rendering-introduces-next-era-of-ai-powered-graphics-innovation/

**What it is:** Tiny neural networks (small MLPs, typically 2–4 layers, 16–64 neurons wide) compiled into GPU shader code and executed inline alongside traditional GLSL/HLSL instructions.

**Applications:**
- **Neural Texture Compression:** compress complex multi-layer texture stacks (PBR albedo + normal + roughness + metallic) into a compact neural representation, decompress at shader time. Up to 5× smaller VRAM footprint.
- **Neural Materials:** compress film-quality material shaders (skin, fabric, complex BRDFs requiring many layers) into a compact neural form. Material processing up to 5× faster.
- **Neural Radiance Cache (NRC):** trains during play, learning scene-specific indirect lighting. Used in RTX Remix.
- **Generative Adversarial Shaders (research, arxiv:2306.04629):** GAN-trained shader for realism enhancement — still pre-production research.

**Engineering implication for SpaceGen (Three.js/WebGL):** Direct WebGL equivalent is not yet possible — WebGL does not support cooperative vectors. However:
- WebGPU (the successor API) has compute shader support that could enable MLP evaluation in a fragment/compute pass without hardware acceleration.
- The conceptual model — "small MLP trained offline, run at shader-time per-pixel" — is applicable today in WebGPU with a handwritten MLP in WGSL.
- Timeline: mainstream WebGPU support + neural shader techniques 2025–2027.

---

### 2. Real-Time Stable Diffusion in Live Performance

**Tools:** StreamDiffusion (ComfyUI plugin), Deforum, img2img in real-time  
**Performance:** 5–30 fps on RTX 3090/4090 with StreamDiffusion (varies by prompt complexity and resolution)  

**Live workflows in use (2024–2025):**
- TouchDesigner → Spout → ComfyUI StreamDiffusion (img2img, fixed prompt, low denoising strength ~0.4) → Spout back → TD → LED wall.
- Low denoising strength preserves generative structure while adding AI painterly texture.
- Fixed seed + consistent prompt = stylistically coherent output; randomized seed = chaotic variation on beat.

**Notable deployments:**
- Jean-Michel Jarre at Starmus Festival: AI-generated imagery projected onto cityscape, synchronized with lasers and drones.
- Jordan Rudess MIT Media Lab performance (September 21, 2024): AI-generated music + 16-foot kinetic sculpture visualizing AI output in real-time.
- Anyma at Sphere: "AI-driven generative art: ML algorithms used to create procedural animations that reacted dynamically to the music."
- Samsung Galaxy AI Immersive Music Festival (Ho Chi Minh City, Summer 2024): mobile phone ARKit facial motion capture → Notch 1.0 effects applied to performer faces in real-time.

---

### 3. XR LED Volume Stages in Concerts

**Technology stack:**
- **LED panels:** ROE Visual, Absen, Roe Black Marble — refresh rate must exceed 3,840 Hz to avoid rolling scan lines on camera. Premium panels reach 7,680 Hz+.
- **Content engine:** Unreal Engine 5 (via nDisplay) or Notch/Disguise.
- **Camera tracking:** Ncam, Mo-Sys, Vicon — feeds real-time camera pose to render engine → parallax-correct background.
- **Latency budget:** 10ms industry standard maximum for parallax-free XR on-camera.

**Concert use (2024–2025):**
- Taylor Swift "Eras Tour": Disguise GX3 + Notch Blocks, massive LED wall arrays.
- Coldplay "Music of the Spheres": Disguise pipeline + XYLOBANDS (LED wristbands on 80,000+ audience members, each synced to stage content via radio).
- Eurovision 2024: Disguise media server backbone.
- VMAs 2024: Disguise platform.
- Phish at Sphere (2024): Unreal Engine 5 + Moment Factory, DMX-controlled real-time content.
- Anyma "End of Genesys" at Sphere (NYE 2024): UE5 Nanite/Lumen, 16K 360° wrap-around display.

**LED panels as generative light sources (emerging technique 2024–2025):**
- Slow-moving gradient and color-field shaders on LED wall panels provide colored wash light for performers — eliminating or supplementing traditional front-light fixtures.
- High-brightness LED tiles that double as environmental wash fixtures by driving solid colors or slow-moving gradients.

**Pixel mapping technical workflow:**
- Media server (Resolume Arena, Disguise, Hippotizer, Millumin) maps canvas regions to individual LED panel processor outputs via Art-Net/sACN DMX.
- Pixel-accurate canvas mirrors the physical layout of panels on stage.
- Disconnected LED columns / ceiling canopy: mapping software slices the master video and routes correct pixels to each processor output.

---

### 4. Real-Time ML Performer Tracking

**Body tracking → visual driver:**
- Notch (built-in), TouchDesigner (Kinect/RealSense/MediaPipe CHOP), Unreal Engine (with custom ML plugin) track performers' skeleton in real-time.
- Joint positions drive: particle emitter locations, shader parameters, generative geometry attachment points.
- Latency: Kinect/RealSense ~15ms; MediaPipe webcam ~30ms; Vicon suit ~5ms.

**Face tracking:**
- Apple ARKit face data (from iPhone), Notch face tracking, mobile phone ARKit → feed facial expression coefficients (52 blendshapes) into shader parameters / Notch node parameters.
- Samsung Galaxy AI festival: "mobile phone-based facial motion capture → exported to Notch to add effects."

**Optical flow:**
- Analyze camera input frame-to-frame motion → vector field advecting particles or displacing textures.
- Available in Notch and TouchDesigner natively.
- Artists: feed live camera of crowd or performer, use optical flow to drive particle system direction.

---

### 5. Live Coding Scene (Algorave / TOPLAP)

**Community:** TOPLAP (toplap.org) — founded 2004; Algorave events globally.

**Key tools:**
| Tool | What it does | URL |
|------|-------------|-----|
| Hydra | Browser-based WebGL video synth / live-coding | hydra.ojack.xyz |
| TidalCycles | Music live-coding (Haskell DSL), often paired with Hydra visuals | tidalcycles.org |
| Flok | Web-based P2P collaborative live-coding: multiple coders drive Hydra/TidalCycles in a single performance | flok.cc |
| VEDA | Real-time GLSL editor plugin for Atom — raw shader control | github.com/fand/veda |
| SuperCollider | Audio live-coding, often paired with Processing/oF/GLSL visuals | supercollider.github.io |
| KodeLife | GLSL live editor, Syphon output to VJ software | hexler.net/kodelife |

**Revision 2024 Shader Showdown:**
- Live shader coding competition at Revision demoparty.
- Participants write GLSL from scratch in 25 minutes, judged visually.
- YouTube: "Revision 2024 - Shader Showdown Qualifications" — excellent reference for what's achievable in dense, optimized GLSL.

---

### 6. Demoscene Shader Techniques

The demoscene (Revision, Evoke, Assembly, Chaos Constructions) continues pushing realtime shader art forward:
- **4KB/64KB intros:** entire audiovisual demo in 4096 bytes — necessarily 100% procedural GLSL.
- Techniques pioneered: SDF raymarching, procedural music via bytebeat, shader-packed geometry with hash-based noise.

**Inigo Quilez's iquilezles.org** remains the single most referenced technical resource for SDF, noise, and color in live shader coding. His "Slisesix" (2008) — first public SDF raymarching demo combining smooth blending, domain repetition, AO, and soft shadows in a 4KB executable — launched the modern SDF era in demoscene and, subsequently, Shadertoy.

---

## Summary for SpaceGen Engineering — Priority Implementation List

Based on this survey of 14 software platforms, production documentation from major live events, and shader library analysis, the highest-priority implementations for SpaceGen to be competitive with professional VJ platforms are:

| Priority | Technique | Why | Reference |
|----------|-----------|-----|-----------|
| P0 | fBm + Domain Warping | Universal "organic motion" base layer | iquilezles.org/articles/warp |
| P0 | Cosine Color Palettes | Parameterized color cycling, trivial to implement | iquilezles.org/articles/palettes |
| P0 | Audio FFT → shader uniforms | Sync is #1 differentiator for live shows | Web Audio `AnalyserNode` → `DataTexture` |
| P0 | Chromatic Aberration / RGB Shift | Instant "distress" post-process, one line of GLSL | Resolume / ISF community |
| P1 | SDF Raymarching | Entire 3D scenes in one fragment shader | iquilezles.org/articles/distfunctions |
| P1 | Feedback / Echo Trails | Ping-pong `WebGLRenderTarget` — hypnotic audio reverb | Shadertoy: 4sKBDc |
| P1 | Voronoi / Cellular Patterns | Organic cell textures, neon grid aesthetic | Shadertoy: MslGD8 (iq) |
| P1 | GPU Particle System (GPGPU) | Millions of audio-reactive particles | `GPUComputationRenderer` |
| P2 | Kaleidoscope | Polar UV mirror applied to any source | LYGIA: space/kaleidoscope |
| P2 | Glitch / Datamosh / Pixel Sort | High-energy accent effects | ISF community |
| P2 | Tunnel / Warp Speed | Infinite zoom, polar UV warp | Shadertoy: 4djBRm |
| P3 | Reaction-Diffusion (Gray-Scott) | Slowly evolving Turing patterns | Shadertoy: lXXcz7 |
| P3 | Volumetric Clouds / Fog | Atmospheric depth, expensive but spectacular | Shadertoy: 3l23Rh, XslGRr |
| P3 | Real-time SD integration | AI painterly texture on generative content | StreamDiffusion + Spout |

**Recommended shader library:** LYGIA (lygia.xyz) — `#include "lygia/generative/fbm.glsl"`, `lygia/generative/voronoi.glsl"`, `lygia/color/palette/cosine.glsl"`, `lygia/space/kaleidoscope.glsl"`. Multi-language (GLSL/WGSL/Metal). MIT-compatible for commercial use via Patron tier.

**Key open-source repositories to study and draw from:**

| Repo | URL | Contents |
|------|-----|----------|
| LYGIA | github.com/patriciogonzalezvivo/lygia | Complete shader function library |
| ISF-Files (Vidvox) | github.com/Vidvox/ISF-Files | 300+ VJ-ready GLSL shaders |
| ISF community | github.com/bareimage/ISF | Feedback/animation-optimized ISF shaders |
| ShaderSketches | github.com/keijiro/ShaderSketches | 50+ KodeLife live-performance sketches |
| ofxFX | github.com/patriciogonzalezvivo/ofxFX | GPU effects for oF (blur, bloom, feedback) |
| VL.Fuse | github.com/TheFuseLab/VL.Fuse | vvvv GPU library (SDSL/HLSL) for architectural reference |
| webgl-noise | github.com/ashima/webgl-noise | Zero-texture Simplex noise for GLSL |
| glsl-noise gist | gist.github.com/patriciogonzalezvivo/670c22f3966e662d2f83 | Simplex/Perlin collection |

---

*Research compiled by Tools Agent, SpaceGen Research Team — 2026-05-14*
