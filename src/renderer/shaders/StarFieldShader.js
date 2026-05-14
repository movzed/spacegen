import * as THREE from 'three'

const vert = `
varying vec2 vUv;
void main() { vUv = uv; gl_Position = vec4(position, 1.0); }
`

const frag = `
precision highp float;

uniform float uTime;
uniform float uOpacity;
uniform sampler2D uMask;
uniform bool uHasMask;

uniform float uDensity;
uniform float uSpeed;
uniform float uAngle;    // degrees
uniform float uTwinkle;
uniform float uStreak;
uniform vec3  uColorA;
uniform vec3  uColorB;
uniform float uBrightness;
uniform float uBeatFlash;  // 0.0 normally, 3.0 on beat flash

varying vec2 vUv;

// ------------------------------------------------------------------
// Hash helpers
// ------------------------------------------------------------------
float hash11(float n) { return fract(sin(n) * 43758.5453123); }
float hash21(vec2 p)  { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453123); }
vec2  hash22(vec2 p)  {
  p = vec2(dot(p, vec2(127.1, 311.7)), dot(p, vec2(269.5, 183.3)));
  return fract(sin(p) * 43758.5453123);
}

// ------------------------------------------------------------------
// Single-layer star field.
//   uv        - screen UV (already scrolled for this layer)
//   density   - cells per unit (e.g. 80)
//   size      - star dot radius in cell space [0..1]
//   dir       - normalised scroll direction (for streak)
// Returns brightness in [0,1].
// ------------------------------------------------------------------
float stars(vec2 uv, float density, float size, vec2 dir) {
  vec2 cell  = floor(uv * density);
  vec2 local = fract(uv * density);          // [0,1] within cell

  float id   = hash21(cell);
  // Jitter center within [0.1, 0.9] to avoid border clipping
  vec2 center = 0.1 + 0.8 * hash22(cell + 13.7);

  vec2 delta = local - center;

  // Streak: compress in perpendicular to dir, elongate along dir
  float streakFactor = 1.0 + uStreak * 6.0;
  vec2 perp = vec2(-dir.y, dir.x);
  float dAlong = dot(delta, dir);
  float dPerp  = dot(delta, perp);
  // Compress perp, elongate along
  vec2 delta2 = dAlong * dir / streakFactor + dPerp * perp;

  float dist  = length(delta2);
  float star  = smoothstep(size, 0.0, dist);

  // Twinkle
  float twinkle = mix(1.0, 0.8 + 0.2 * sin(uTime * 3.0 + id * 6.28318), uTwinkle);

  return star * twinkle;
}

void main() {
  float rad = uAngle * 3.14159265 / 180.0;
  vec2  dir = vec2(cos(rad), sin(rad));

  // Layer A — fast (speed 1x), small stars, colorA
  vec2 uvA = vUv + dir * uTime * uSpeed * 0.12;
  float bA = stars(uvA, uDensity,        0.012, dir);

  // Layer B — medium (speed 0.6x), mid stars, mix colorA/colorB
  vec2 uvB = vUv + dir * uTime * uSpeed * 0.12 * 0.6;
  float bB = stars(uvB, uDensity * 0.65, 0.018, dir);

  // Layer C — slow (speed 0.3x), large stars, colorB
  vec2 uvC = vUv + dir * uTime * uSpeed * 0.12 * 0.3;
  float bC = stars(uvC, uDensity * 0.35, 0.025, dir);

  vec3 col = uColorA * bA
           + mix(uColorA, uColorB, 0.5) * bB
           + uColorB   * bC;

  float brightness = uBrightness * (1.0 + uBeatFlash);
  col *= brightness;

  float maskVal = uHasMask ? texture2D(uMask, vUv).r : 1.0;
  float alpha   = clamp(bA + bB + bC, 0.0, 1.0) * maskVal;

  gl_FragColor = vec4(col, alpha * uOpacity);
}
`

export class StarFieldShader {
  constructor() {
    this._beatFlashTimer = 0.0

    this.uniforms = {
      uTime:       { value: 0 },
      uOpacity:    { value: 1 },
      uMask:       { value: null },
      uHasMask:    { value: false },
      uDensity:    { value: 80.0 },
      uSpeed:      { value: 0.3 },
      uAngle:      { value: 270.0 },
      uTwinkle:    { value: 0.5 },
      uStreak:     { value: 0.0 },
      uColorA:     { value: new THREE.Color('#ffffff') },
      uColorB:     { value: new THREE.Color('#8888ff') },
      uBrightness: { value: 2.0 },
      uBeatFlash:  { value: 0.0 },
    }

    const mat = new THREE.ShaderMaterial({
      vertexShader:   vert,
      fragmentShader: frag,
      uniforms:       this.uniforms,
      transparent:    true,
      depthWrite:     false,
      blending:       THREE.AdditiveBlending,
    })

    this.threeObject = new THREE.Mesh(new THREE.PlaneGeometry(2, 2), mat)
  }

  update(time, delta, beat, bpmClock) {
    this.uniforms.uTime.value = time

    if (beat) {
      this._beatFlashTimer = 0.05  // 50 ms flash
    }

    if (this._beatFlashTimer > 0) {
      this.uniforms.uBeatFlash.value = 2.0  // adds 2x on top of base → 3x total
      this._beatFlashTimer -= delta
    } else {
      this.uniforms.uBeatFlash.value = 0.0
    }
  }

  buildGUI(folder) {
    const u = this.uniforms
    const p = {
      density:    80.0,
      speed:      0.3,
      angle:      270,
      twinkle:    0.5,
      streak:     0.0,
      colorA:     '#ffffff',
      colorB:     '#8888ff',
      brightness: 2.0,
    }
    folder.add(p, 'density', 10, 200, 1).name('Density').onChange(v => u.uDensity.value = v)
    folder.add(p, 'speed', 0, 3, 0.01).name('Speed').onChange(v => u.uSpeed.value = v)
    folder.add(p, 'angle', 0, 360, 1).name('Angle °').onChange(v => u.uAngle.value = v)
    folder.add(p, 'twinkle', 0, 1, 0.01).name('Twinkle').onChange(v => u.uTwinkle.value = v)
    folder.add(p, 'streak', 0, 1, 0.01).name('Streak').onChange(v => u.uStreak.value = v)
    folder.addColor(p, 'colorA').name('Color A').onChange(v => u.uColorA.value.set(v))
    folder.addColor(p, 'colorB').name('Color B').onChange(v => u.uColorB.value.set(v))
    folder.add(p, 'brightness', 0, 5, 0.01).name('Brightness').onChange(v => u.uBrightness.value = v)
  }
}
