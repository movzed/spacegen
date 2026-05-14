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

uniform float uFreq1;
uniform float uFreq2;
uniform float uFreq3;
uniform float uFreq4;
uniform float uSpeed;
uniform float uBrightness;
uniform vec3  uColorA;
uniform vec3  uColorB;
uniform vec3  uColorC;
uniform vec3  uColorD;

varying vec2 vUv;

vec3 palette(float t, vec3 a, vec3 b, vec3 c, vec3 d) {
  return a + b * cos(6.28318 * (c * t + d));
}

void main() {
  vec2 uv = vUv;

  float v = sin(uv.x * uFreq1 + uTime * uSpeed)
           + sin(uv.y * uFreq2 + uTime * uSpeed * 0.7)
           + sin((uv.x + uv.y) * uFreq3 + uTime * uSpeed * 1.3)
           + sin(length(uv - 0.5) * uFreq4 - uTime * uSpeed * 2.0);

  // Normalize v from [-4,4] to [0,1]
  float t = v * 0.125 + 0.5;

  vec3 col = palette(t, uColorA, uColorB, uColorC, uColorD);

  float maskVal = uHasMask ? texture2D(uMask, vUv).r : 1.0;

  gl_FragColor = vec4(col * uBrightness * maskVal, maskVal * uOpacity);
}
`

export class PlasmaWaveShader {
  constructor() {
    this._beatFreq1Base = 5.0

    this.uniforms = {
      uTime:       { value: 0 },
      uOpacity:    { value: 1 },
      uMask:       { value: null },
      uHasMask:    { value: false },
      uFreq1:      { value: 5.0 },
      uFreq2:      { value: 4.0 },
      uFreq3:      { value: 3.0 },
      uFreq4:      { value: 6.0 },
      uSpeed:      { value: 0.5 },
      uBrightness: { value: 1.5 },
      uColorA:     { value: new THREE.Color(0.5, 0.5, 0.5) },
      uColorB:     { value: new THREE.Color(0.5, 0.5, 0.5) },
      uColorC:     { value: new THREE.Color(1.0, 1.0, 1.0) },
      uColorD:     { value: new THREE.Color(0.0, 0.33, 0.67) },
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
      // Spike freq1 by +2 for one frame then restore on next frame
      this._beatFreq1Base = this.uniforms.uFreq1.value
      this.uniforms.uFreq1.value += 2.0
      this._restoreFreq1 = true
    } else if (this._restoreFreq1) {
      this.uniforms.uFreq1.value = this._beatFreq1Base
      this._restoreFreq1 = false
    }
  }

  buildGUI(folder) {
    const u = this.uniforms
    const p = {
      freq1:      5.0,
      freq2:      4.0,
      freq3:      3.0,
      freq4:      6.0,
      speed:      0.5,
      brightness: 1.5,
      colorA:     '#808080',
      colorB:     '#808080',
      colorC:     '#ffffff',
      colorD:     '#005488',
    }
    folder.add(p, 'freq1', 0.1, 20, 0.1).name('Freq 1').onChange(v => {
      u.uFreq1.value = v
      this._beatFreq1Base = v
    })
    folder.add(p, 'freq2', 0.1, 20, 0.1).name('Freq 2').onChange(v => u.uFreq2.value = v)
    folder.add(p, 'freq3', 0.1, 20, 0.1).name('Freq 3').onChange(v => u.uFreq3.value = v)
    folder.add(p, 'freq4', 0.1, 20, 0.1).name('Freq 4').onChange(v => u.uFreq4.value = v)
    folder.add(p, 'speed', 0, 5, 0.01).name('Speed').onChange(v => u.uSpeed.value = v)
    folder.add(p, 'brightness', 0, 5, 0.01).name('Brightness').onChange(v => u.uBrightness.value = v)
    folder.addColor(p, 'colorA').name('Color A').onChange(v => u.uColorA.value.set(v))
    folder.addColor(p, 'colorB').name('Color B').onChange(v => u.uColorB.value.set(v))
    folder.addColor(p, 'colorC').name('Color C').onChange(v => u.uColorC.value.set(v))
    folder.addColor(p, 'colorD').name('Color D').onChange(v => u.uColorD.value.set(v))
  }
}
