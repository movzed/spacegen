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

uniform float uAberration;
uniform float uThreshold;
uniform float uShiftAmount;
uniform float uGlitchSpeed;
uniform float uLines;
uniform float uScanlineIntensity;
uniform float uScanlineCount;
uniform float uBrightness;

varying vec2 vUv;

float rand(vec2 c) {
  return fract(sin(dot(c, vec2(12.9898, 78.233))) * 43758.5453);
}

void main() {
  float lineId = floor(vUv.y * uLines);
  float rng    = rand(vec2(lineId, floor(uTime * uGlitchSpeed)));
  float glitch = step(uThreshold, rng);
  float shift  = (rand(vec2(lineId * 7.3, uTime * uGlitchSpeed)) - 0.5)
                 * uShiftAmount * glitch;

  vec2 uvR = vec2(vUv.x + shift + uAberration, vUv.y);
  vec2 uvG = vec2(vUv.x + shift,               vUv.y);
  vec2 uvB = vec2(vUv.x + shift - uAberration, vUv.y);

  float r, g, b;
  if (uHasMask) {
    r = texture2D(uMask, fract(uvR)).r;
    g = texture2D(uMask, fract(uvG)).g;
    b = texture2D(uMask, fract(uvB)).b;
  } else {
    float t = floor(uTime * uGlitchSpeed);
    r = step(0.5, rand(floor(uvR * 40.0) + t));
    g = step(0.5, rand(floor(uvG * 40.0) + t * 1.1));
    b = step(0.5, rand(floor(uvB * 40.0) + t * 0.9));
  }

  float scanline = 1.0 - uScanlineIntensity * step(0.5, fract(vUv.y * uScanlineCount));

  vec3 col = vec3(r, g, b) * scanline * uBrightness;

  gl_FragColor = vec4(col, uOpacity);
}
`

export class ChromaGlitchShader {
  constructor() {
    this._baseThreshold = 0.92
    this._beatFrame     = false

    this.uniforms = {
      uTime:              { value: 0 },
      uOpacity:           { value: 1 },
      uMask:              { value: null },
      uHasMask:           { value: false },
      uAberration:        { value: 0.01 },
      uThreshold:         { value: 0.92 },
      uShiftAmount:       { value: 0.05 },
      uGlitchSpeed:       { value: 3.0 },
      uLines:             { value: 30.0 },
      uScanlineIntensity: { value: 0.15 },
      uScanlineCount:     { value: 240.0 },
      uBrightness:        { value: 1.5 },
    }

    const mat = new THREE.ShaderMaterial({
      vertexShader: vert,
      fragmentShader: frag,
      uniforms: this.uniforms,
      transparent: true,
      depthWrite: false,
      blending: THREE.AdditiveBlending,
    })

    this.threeObject = new THREE.Mesh(new THREE.PlaneGeometry(2, 2), mat)
  }

  update(time, delta, beat) {
    this.uniforms.uTime.value = time

    if (beat) {
      this.uniforms.uThreshold.value = 0.5
      this._beatFrame = true
    } else if (this._beatFrame) {
      this.uniforms.uThreshold.value = this._baseThreshold
      this._beatFrame = false
    }
  }

  buildGUI(folder) {
    const u = this.uniforms
    const p = {
      aberration: 0.01, threshold: 0.92, shiftAmount: 0.05,
      glitchSpeed: 3.0, lines: 30.0, scanlineIntensity: 0.15,
      brightness: 1.5,
    }
    folder.add(p, 'aberration', 0.0, 0.1, 0.001).name('Aberration').onChange(v => u.uAberration.value = v)
    folder.add(p, 'threshold', 0.0, 1.0, 0.01).name('Threshold').onChange(v => {
      this._baseThreshold = v
      u.uThreshold.value  = v
    })
    folder.add(p, 'shiftAmount', 0.0, 0.3, 0.001).name('Shift Amount').onChange(v => u.uShiftAmount.value = v)
    folder.add(p, 'glitchSpeed', 0.5, 20.0, 0.1).name('Glitch Speed').onChange(v => u.uGlitchSpeed.value = v)
    folder.add(p, 'lines', 5.0, 200.0, 1.0).name('Lines').onChange(v => u.uLines.value = v)
    folder.add(p, 'scanlineIntensity', 0.0, 1.0, 0.01).name('Scanline').onChange(v => u.uScanlineIntensity.value = v)
    folder.add(p, 'brightness', 0.0, 5.0, 0.01).name('Brightness').onChange(v => u.uBrightness.value = v)
  }
}
