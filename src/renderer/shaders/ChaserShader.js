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
uniform sampler2D uTexture;
uniform bool uHasTexture;

uniform float uSpeed;
uniform float uAngle;       // degrees → shader converts to radians
uniform float uWidth;
uniform float uTrail;
uniform float uCount;
uniform float uDistortion;
uniform float uDistortScale;
uniform float uDistortSpeed;
uniform vec3  uColorHead;
uniform vec3  uColorTail;
uniform float uBrightness;

varying vec2 vUv;

float rand(vec2 c) { return fract(sin(dot(c, vec2(12.9898,78.233)))*43758.5453); }
float noise(vec2 p) {
  vec2 i = floor(p), f = fract(p);
  f = f*f*(3.0-2.0*f);
  return mix(mix(rand(i),rand(i+vec2(1,0)),f.x), mix(rand(i+vec2(0,1)),rand(i+vec2(1,1)),f.x), f.y);
}
float fbm(vec2 p) {
  float v=0.0, a=0.5;
  for(int i=0;i<4;i++){ v+=a*noise(p); p*=2.1; a*=0.5; }
  return v;
}

void main() {
  // Distortion
  vec2 uv = vUv;
  float n  = fbm(uv * uDistortScale + vec2(0.0, uTime * uDistortSpeed));
  float n2 = fbm(uv * uDistortScale * 0.7 + vec2(uTime * uDistortSpeed * 0.8, 3.4));
  uv.x += (n  - 0.5) * uDistortion * 0.08;
  uv.y += (n2 - 0.5) * uDistortion * 0.08;

  float maskVal = uHasMask ? texture2D(uMask, vUv).r : 1.0;

  float rad = uAngle * 3.14159265 / 180.0;
  vec2  dir = vec2(cos(rad), sin(rad));
  float pos = dot(uv, dir);

  vec3  col   = vec3(0.0);
  float alpha = 0.0;

  for(float i=0.0; i<8.0; i++) {
    if(i >= uCount) break;
    float offset  = i / uCount;
    float chaser  = fract(pos - uTime * uSpeed * 0.12 + offset);
    float head    = smoothstep(uWidth, 0.0, chaser);
    float trail   = smoothstep(0.0, uWidth * max(uTrail, 0.001), chaser);
    float band    = head * trail;
    col   += mix(uColorTail, uColorHead, head) * band;
    alpha += band;
  }
  alpha = clamp(alpha, 0.0, 1.0);

  // Texture overlay
  if(uHasTexture) {
    vec4 tex = texture2D(uTexture, uv + vec2(uTime * 0.04, 0.0));
    col *= tex.rgb * 2.0;
  }

  gl_FragColor = vec4(col * uBrightness, alpha * maskVal * uOpacity);
}
`

export class ChaserShader {
  constructor() {
    this.uniforms = {
      uTime:        { value: 0 },
      uOpacity:     { value: 1 },
      uMask:        { value: null },
      uHasMask:     { value: false },
      uTexture:     { value: null },
      uHasTexture:  { value: false },
      uSpeed:       { value: 0.5 },
      uAngle:       { value: 0 },
      uWidth:       { value: 0.12 },
      uTrail:       { value: 0.75 },
      uCount:       { value: 1 },
      uDistortion:  { value: 0.3 },
      uDistortScale:{ value: 2.0 },
      uDistortSpeed:{ value: 0.5 },
      uColorHead:   { value: new THREE.Color('#ffffff') },
      uColorTail:   { value: new THREE.Color('#0044ff') },
      uBrightness:  { value: 2.0 },
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

  update(time) { this.uniforms.uTime.value = time }

  buildGUI(folder) {
    const u = this.uniforms
    const p = {
      speed: 0.5, angle: 0, width: 0.12, trail: 0.75, count: 1,
      distortion: 0.3, distortScale: 2.0, distortSpeed: 0.5,
      colorHead: '#ffffff', colorTail: '#0044ff', brightness: 2.0,
    }
    folder.add(p,'speed',0,5,0.01).name('Speed').onChange(v=>u.uSpeed.value=v)
    folder.add(p,'angle',0,360,1).name('Angle °').onChange(v=>u.uAngle.value=v)
    folder.add(p,'width',0.01,0.5,0.001).name('Width').onChange(v=>u.uWidth.value=v)
    folder.add(p,'trail',0,1,0.01).name('Trail').onChange(v=>u.uTrail.value=v)
    folder.add(p,'count',1,8,1).name('Count').onChange(v=>u.uCount.value=v)
    folder.add(p,'distortion',0,1,0.01).name('Distortion').onChange(v=>u.uDistortion.value=v)
    folder.add(p,'distortScale',0.5,10,0.1).name('Dist Scale').onChange(v=>u.uDistortScale.value=v)
    folder.add(p,'distortSpeed',0,3,0.01).name('Dist Speed').onChange(v=>u.uDistortSpeed.value=v)
    folder.addColor(p,'colorHead').name('Head Color').onChange(v=>u.uColorHead.value.set(v))
    folder.addColor(p,'colorTail').name('Tail Color').onChange(v=>u.uColorTail.value.set(v))
    folder.add(p,'brightness',0,5,0.01).name('Brightness').onChange(v=>u.uBrightness.value=v)
  }
}
