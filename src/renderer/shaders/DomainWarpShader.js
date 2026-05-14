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

uniform float uScale;
uniform float uWarpStrength;
uniform float uTimeScale;
uniform float uBrightness;
uniform vec3  uColorLow;
uniform vec3  uColorMid;
uniform vec3  uColorHigh;

varying vec2 vUv;

// ------------------------------------------------------------------
// Value noise + 5-octave FBM (same helpers as ChaserShader)
// ------------------------------------------------------------------
float rand(vec2 c) { return fract(sin(dot(c, vec2(12.9898, 78.233))) * 43758.5453); }

float noise(vec2 p) {
  vec2 i = floor(p);
  vec2 f = fract(p);
  f = f * f * (3.0 - 2.0 * f);
  return mix(
    mix(rand(i),               rand(i + vec2(1.0, 0.0)), f.x),
    mix(rand(i + vec2(0.0, 1.0)), rand(i + vec2(1.0, 1.0)), f.x),
    f.y
  );
}

float fbm(vec2 p) {
  float v = 0.0;
  float a = 0.5;
  for (int i = 0; i < 5; i++) {
    v += a * noise(p);
    p *= 2.1;
    a *= 0.5;
  }
  return v;
}

void main() {
  vec2 uv = vUv * uScale;

  // Two-layer domain warp (Inigo Quilez)
  vec2 q = vec2(
    fbm(uv                               + uTime * uTimeScale),
    fbm(uv + vec2(5.2, 1.3)             + uTime * uTimeScale)
  );

  vec2 r = vec2(
    fbm(uv + uWarpStrength * q + vec2(1.7, 9.2)  + 0.150 * uTime * uTimeScale),
    fbm(uv + uWarpStrength * q + vec2(8.3, 2.8)  + 0.126 * uTime * uTimeScale)
  );

  float f = fbm(uv + uWarpStrength * r);

  // 3-color gradient: low → mid → high
  vec3 col = mix(
    mix(uColorLow, uColorMid,  clamp(f * 2.0,       0.0, 1.0)),
    uColorHigh,
    clamp(f * 2.0 - 1.0, 0.0, 1.0)
  );
  col *= f * 2.5;  // contrast boost

  float maskVal = uHasMask ? texture2D(uMask, vUv).r : 1.0;

  gl_FragColor = vec4(col * uBrightness * maskVal, maskVal * uOpacity);
}
`

export class DomainWarpShader {
  constructor() {
    this._warpStrengthBase = 1.2
    this._restoreWarp = false

    this.uniforms = {
      uTime:         { value: 0 },
      uOpacity:      { value: 1 },
      uMask:         { value: null },
      uHasMask:      { value: false },
      uScale:        { value: 1.5 },
      uWarpStrength: { value: 1.2 },
      uTimeScale:    { value: 0.12 },
      uBrightness:   { value: 2.0 },
      uColorLow:     { value: new THREE.Color('#0d0221') },
      uColorMid:     { value: new THREE.Color('#341569') },
      uColorHigh:    { value: new THREE.Color('#ff6ad5') },
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
      this._warpStrengthBase = this.uniforms.uWarpStrength.value
      this.uniforms.uWarpStrength.value = 2.5
      this._restoreWarp = true
    } else if (this._restoreWarp) {
      this.uniforms.uWarpStrength.value = this._warpStrengthBase
      this._restoreWarp = false
    }
  }

  buildGUI(folder) {
    const u = this.uniforms
    const p = {
      scale:        1.5,
      warpStrength: 1.2,
      timeScale:    0.12,
      brightness:   2.0,
      colorLow:     '#0d0221',
      colorMid:     '#341569',
      colorHigh:    '#ff6ad5',
    }
    folder.add(p, 'scale', 0.1, 5, 0.01).name('Scale').onChange(v => u.uScale.value = v)
    folder.add(p, 'warpStrength', 0, 5, 0.01).name('Warp Strength').onChange(v => {
      u.uWarpStrength.value = v
      this._warpStrengthBase = v
    })
    folder.add(p, 'timeScale', 0, 1, 0.001).name('Time Scale').onChange(v => u.uTimeScale.value = v)
    folder.add(p, 'brightness', 0, 5, 0.01).name('Brightness').onChange(v => u.uBrightness.value = v)
    folder.addColor(p, 'colorLow').name('Color Low').onChange(v => u.uColorLow.value.set(v))
    folder.addColor(p, 'colorMid').name('Color Mid').onChange(v => u.uColorMid.value.set(v))
    folder.addColor(p, 'colorHigh').name('Color High').onChange(v => u.uColorHigh.value.set(v))
  }
}
