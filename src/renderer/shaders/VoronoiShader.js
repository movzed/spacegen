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
uniform float uSpeed;
uniform float uBorderWidth;
uniform float uFill;
uniform float uGlow;
uniform vec3  uColorA;
uniform vec3  uColorB;
uniform vec3  uBorderColor;

varying vec2 vUv;

vec2 rand2(vec2 p) {
  return fract(sin(vec2(
    dot(p, vec2(127.1, 311.7)),
    dot(p, vec2(269.5, 183.3))
  )) * 43758.5453);
}

void main() {
  float m = uHasMask ? texture2D(uMask, vUv).r : 1.0;

  vec2 scaledUv = vUv * uScale;
  vec2 cellId   = floor(scaledUv);
  vec2 cellUv   = fract(scaledUv);

  float minDist  = 8.0;
  vec2  minPoint = vec2(0.0);

  for (int x = -1; x <= 1; x++) {
    for (int y = -1; y <= 1; y++) {
      vec2 n    = vec2(float(x), float(y));
      vec2 seed = rand2(cellId + n);
      vec2 point = n + seed * 0.5 + 0.5
                   + 0.4 * sin(uTime * uSpeed * 0.5 + seed * 6.28318);
      float d = length(cellUv - point);
      if (d < minDist) {
        minDist  = d;
        minPoint = seed;
      }
    }
  }

  float border    = 1.0 - smoothstep(0.0, uBorderWidth, minDist);
  float cellFill  = smoothstep(0.4, 0.5, minDist);
  vec3  cellColor = mix(uColorA, uColorB, rand2(minPoint).x);

  vec3 col;
  if (uFill > 0.5) {
    col = cellColor * cellFill + uBorderColor * border;
  } else {
    col = uBorderColor * border;
  }
  col *= (1.0 + border * uGlow);

  gl_FragColor = vec4(col * m, uOpacity * m);
}
`

export class VoronoiShader {
  constructor() {
    this._scaleSpiked = false

    this.uniforms = {
      uTime:        { value: 0 },
      uOpacity:     { value: 1 },
      uMask:        { value: null },
      uHasMask:     { value: false },
      uScale:       { value: 5.0 },
      uSpeed:       { value: 1.0 },
      uBorderWidth: { value: 0.04 },
      uFill:        { value: 1.0 },
      uGlow:        { value: 2.0 },
      uColorA:      { value: new THREE.Color('#ff6ad5') },
      uColorB:      { value: new THREE.Color('#0000ff') },
      uBorderColor: { value: new THREE.Color('#ffffff') },
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

    this._baseScale = this.uniforms.uScale.value
  }

  update(time, delta, beat) {
    this.uniforms.uTime.value = time

    if (beat) {
      this._baseScale = this.uniforms.uScale.value
      this.uniforms.uScale.value = this._baseScale + 1.0
      this._scaleSpiked = true
    } else if (this._scaleSpiked) {
      this.uniforms.uScale.value = this._baseScale
      this._scaleSpiked = false
    }
  }

  buildGUI(folder) {
    const u = this.uniforms
    const p = {
      scale: 5.0, speed: 1.0, borderWidth: 0.04,
      fill: true, glow: 2.0,
      colorA: '#ff6ad5', colorB: '#0000ff', borderColor: '#ffffff',
    }
    folder.add(p, 'scale', 1.0, 20.0, 0.1).name('Scale').onChange(v => {
      this._baseScale = v
      u.uScale.value = v
    })
    folder.add(p, 'speed', 0.0, 5.0, 0.01).name('Speed').onChange(v => u.uSpeed.value = v)
    folder.add(p, 'borderWidth', 0.005, 0.2, 0.001).name('Border Width').onChange(v => u.uBorderWidth.value = v)
    folder.add(p, 'fill').name('Fill').onChange(v => u.uFill.value = v ? 1.0 : 0.0)
    folder.add(p, 'glow', 0.0, 8.0, 0.1).name('Glow').onChange(v => u.uGlow.value = v)
    folder.addColor(p, 'colorA').name('Color A').onChange(v => u.uColorA.value.set(v))
    folder.addColor(p, 'colorB').name('Color B').onChange(v => u.uColorB.value.set(v))
    folder.addColor(p, 'borderColor').name('Border Color').onChange(v => u.uBorderColor.value.set(v))
  }
}
