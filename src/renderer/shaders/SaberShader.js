import * as THREE from 'three'

const vert = `
varying vec2 vUv;
void main() { vUv = uv; gl_Position = vec4(position, 1.0); }
`

// Saber effect: Sobel edge detection → layered radial glow → animated energy plasma
const frag = `
precision highp float;

uniform float uTime;
uniform float uOpacity;
uniform sampler2D uMask;
uniform bool uHasMask;
uniform vec2  uResolution;

uniform vec3  uCoreColor;
uniform vec3  uGlowColor;
uniform float uGlowRadius;   // pixels
uniform float uCoreIntensity;
uniform float uGlowIntensity;
uniform float uFlicker;
uniform float uFlickerSpeed;
uniform float uEnergy;
uniform float uEnergyScale;
uniform float uEnergySpeed;

varying vec2 vUv;

float rand(vec2 c) { return fract(sin(dot(c,vec2(12.9898,78.233)))*43758.5453); }
float noise(vec2 p) {
  vec2 i=floor(p), f=fract(p);
  f=f*f*(3.0-2.0*f);
  return mix(mix(rand(i),rand(i+vec2(1,0)),f.x), mix(rand(i+vec2(0,1)),rand(i+vec2(1,1)),f.x),f.y);
}

float edge(vec2 uv, vec2 ts) {
  float tl=texture2D(uMask,uv+ts*vec2(-1,-1)).r, tm=texture2D(uMask,uv+ts*vec2(0,-1)).r,
        tr=texture2D(uMask,uv+ts*vec2( 1,-1)).r, ml=texture2D(uMask,uv+ts*vec2(-1, 0)).r,
        mr=texture2D(uMask,uv+ts*vec2( 1, 0)).r, bl=texture2D(uMask,uv+ts*vec2(-1, 1)).r,
        bm=texture2D(uMask,uv+ts*vec2( 0, 1)).r, br=texture2D(uMask,uv+ts*vec2( 1, 1)).r;
  float gx = -tl-2.0*ml-bl+tr+2.0*mr+br;
  float gy = -tl-2.0*tm-tr+bl+2.0*bm+br;
  return clamp(sqrt(gx*gx+gy*gy), 0.0, 1.0);
}

// Box-blur of the edge map to approximate distance glow
float glowBlur(vec2 uv, vec2 ts, float radius) {
  float acc=0.0; float cnt=0.0;
  float step = radius/5.0;
  for(float dx=-radius; dx<=radius; dx+=step) {
    for(float dy=-radius; dy<=radius; dy+=step) {
      acc += edge(uv + vec2(dx,dy)*ts, ts);
      cnt += 1.0;
    }
  }
  return acc/cnt;
}

void main() {
  if(!uHasMask) { gl_FragColor=vec4(0.0); return; }

  vec2 ts = 1.0/uResolution;

  float e  = edge(vUv, ts);
  float g  = pow(clamp(glowBlur(vUv, ts, uGlowRadius), 0.0, 1.0), 0.45);

  // Plasma energy on edge
  float en = noise(vUv * uEnergyScale + vec2(uTime*uEnergySpeed, -uTime*uEnergySpeed*0.6));
  en = en * 2.0 - 1.0;
  float energyVal = e * (0.5 + en) * uEnergy;

  // Flicker
  float fl = 1.0 + sin(uTime * uFlickerSpeed * 6.2832) * uFlicker * 0.4;
  fl *= 1.0 + noise(vec2(uTime*uFlickerSpeed*13.0, 0.5)) * uFlicker * 0.4;

  vec3 color  = uGlowColor * g * uGlowIntensity;
  color      += uCoreColor * (e + clamp(energyVal,0.0,1.0)) * uCoreIntensity * fl;

  float alpha = clamp(g*uGlowIntensity*0.5 + e*uCoreIntensity, 0.0, 1.0) * uOpacity;
  gl_FragColor = vec4(color, alpha);
}
`

export class SaberShader {
  constructor(renderer) {
    const size = renderer
      ? renderer.getSize(new THREE.Vector2())
      : new THREE.Vector2(1920, 1080)

    this.uniforms = {
      uTime:          { value: 0 },
      uOpacity:       { value: 1 },
      uMask:          { value: null },
      uHasMask:       { value: false },
      uResolution:    { value: size },
      uCoreColor:     { value: new THREE.Color('#ffffff') },
      uGlowColor:     { value: new THREE.Color('#0088ff') },
      uGlowRadius:    { value: 8 },
      uCoreIntensity: { value: 3.0 },
      uGlowIntensity: { value: 1.5 },
      uFlicker:       { value: 0.2 },
      uFlickerSpeed:  { value: 8 },
      uEnergy:        { value: 0.8 },
      uEnergyScale:   { value: 8 },
      uEnergySpeed:   { value: 2 },
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

  onResize(w, h) { this.uniforms.uResolution.value.set(w, h) }

  buildGUI(folder) {
    const u = this.uniforms
    const p = {
      glowRadius: 8, coreColor: '#ffffff', glowColor: '#0088ff',
      coreIntensity: 3.0, glowIntensity: 1.5, flicker: 0.2, flickerSpeed: 8,
      energy: 0.8, energyScale: 8, energySpeed: 2,
    }
    folder.add(p,'glowRadius',1,32,0.5).name('Glow Radius').onChange(v=>u.uGlowRadius.value=v)
    folder.addColor(p,'coreColor').name('Core Color').onChange(v=>u.uCoreColor.value.set(v))
    folder.addColor(p,'glowColor').name('Glow Color').onChange(v=>u.uGlowColor.value.set(v))
    folder.add(p,'coreIntensity',0,10,0.1).name('Core Intensity').onChange(v=>u.uCoreIntensity.value=v)
    folder.add(p,'glowIntensity',0,5,0.1).name('Glow Intensity').onChange(v=>u.uGlowIntensity.value=v)
    folder.add(p,'flicker',0,1,0.01).name('Flicker').onChange(v=>u.uFlicker.value=v)
    folder.add(p,'flickerSpeed',0,30,0.5).name('Flicker Speed').onChange(v=>u.uFlickerSpeed.value=v)
    folder.add(p,'energy',0,3,0.01).name('Energy').onChange(v=>u.uEnergy.value=v)
    folder.add(p,'energyScale',1,30,0.5).name('Energy Scale').onChange(v=>u.uEnergyScale.value=v)
    folder.add(p,'energySpeed',0,10,0.1).name('Energy Speed').onChange(v=>u.uEnergySpeed.value=v)
  }
}
