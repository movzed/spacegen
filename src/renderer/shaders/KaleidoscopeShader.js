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

uniform float uSegments;
uniform float uZoom;
uniform float uRotSpeed;
uniform float uRotPhase;
uniform float uOffsetX;
uniform float uOffsetY;
uniform vec3  uColorA;
uniform vec3  uColorB;
uniform vec3  uColorC;
uniform vec3  uColorD;
uniform float uBrightness;

varying vec2 vUv;

mat2 rot2D(float a) {
  float s = sin(a), c = cos(a);
  return mat2(c, -s, s, c);
}

vec3 palette(float t, vec3 a, vec3 b, vec3 c, vec3 d) {
  return a + b * cos(6.28318 * (c * t + d));
}

void main() {
  vec2 uv = vUv - 0.5;
  uv *= uZoom;
  uv = rot2D(uTime * uRotSpeed + uRotPhase) * uv;

  float angle = atan(uv.y, uv.x);
  float r = length(uv);

  float slice = 6.28318 / uSegments;
  angle = mod(angle, slice);
  if (angle > slice * 0.5) angle = slice - angle;

  uv = vec2(cos(angle), sin(angle)) * r;
  uv += vec2(uOffsetX, uOffsetY) + 0.5;

  vec3 col;
  if (uHasMask) {
    col = texture2D(uMask, fract(uv)).rgb;
  } else {
    float v = sin(uv.x * 6.0 + uTime)
            + sin(uv.y * 5.0 + uTime * 0.7)
            + sin(length(uv - 0.5) * 8.0 - uTime * 1.5);
    col = palette(v * 0.5 + 0.5, uColorA, uColorB, uColorC, uColorD);
  }

  col *= uBrightness;

  float m = uHasMask ? 1.0 : 1.0; // mask already used as source above
  gl_FragColor = vec4(col, uOpacity);
}
`

export class KaleidoscopeShader {
  constructor() {
    this._rotPhase = 0.0

    this.uniforms = {
      uTime:      { value: 0 },
      uOpacity:   { value: 1 },
      uMask:      { value: null },
      uHasMask:   { value: false },
      uSegments:  { value: 6.0 },
      uZoom:      { value: 1.5 },
      uRotSpeed:  { value: 0.1 },
      uRotPhase:  { value: 0.0 },
      uOffsetX:   { value: 0.0 },
      uOffsetY:   { value: 0.0 },
      uColorA:    { value: new THREE.Color(0.5, 0.5, 0.5) },
      uColorB:    { value: new THREE.Color(0.5, 0.5, 0.5) },
      uColorC:    { value: new THREE.Color(1.0, 0.7, 0.4) },
      uColorD:    { value: new THREE.Color(0.0, 0.15, 0.20) },
      uBrightness:{ value: 1.5 },
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
      this._rotPhase += 0.4
      this.uniforms.uRotPhase.value = this._rotPhase
    }
  }

  buildGUI(folder) {
    const u = this.uniforms
    const p = {
      segments: 6, zoom: 1.5, rotSpeed: 0.1,
      offsetX: 0.0, offsetY: 0.0, brightness: 1.5,
      colorA: '#808080', colorB: '#808080',
      colorC: '#ffb366', colorD: '#002633',
    }
    folder.add(p, 'segments', 2, 24, 1).name('Segments').onChange(v => u.uSegments.value = v)
    folder.add(p, 'zoom', 0.1, 5.0, 0.01).name('Zoom').onChange(v => u.uZoom.value = v)
    folder.add(p, 'rotSpeed', -1.0, 1.0, 0.001).name('Rot Speed').onChange(v => u.uRotSpeed.value = v)
    folder.add(p, 'offsetX', -1.0, 1.0, 0.001).name('Offset X').onChange(v => u.uOffsetX.value = v)
    folder.add(p, 'offsetY', -1.0, 1.0, 0.001).name('Offset Y').onChange(v => u.uOffsetY.value = v)
    folder.add(p, 'brightness', 0.0, 5.0, 0.01).name('Brightness').onChange(v => u.uBrightness.value = v)
    folder.addColor(p, 'colorA').name('Palette A').onChange(v => u.uColorA.value.set(v))
    folder.addColor(p, 'colorB').name('Palette B').onChange(v => u.uColorB.value.set(v))
    folder.addColor(p, 'colorC').name('Palette C').onChange(v => u.uColorC.value.set(v))
    folder.addColor(p, 'colorD').name('Palette D').onChange(v => u.uColorD.value.set(v))
  }
}
