import * as THREE from 'three'

// Instanced geometry layer. Each instance is a 2D shape positioned randomly,
// rotated + pulsed over time. Mask drives per-instance alpha via vertex shader
// (requires WebGL2 vertex texture fetches — supported in Electron/Chromium).

const vert = `
precision highp float;
precision highp sampler2D;

attribute float aIdx;       // per-instance 0..N-1

uniform float uTime;
uniform float uN;
uniform float uSize;
uniform float uRotSpeed;
uniform float uPulse;
uniform sampler2D uMask;
uniform bool uHasMask;

varying float vMask;
varying float vEdge;
varying float vIdx;

float rand(float n){ return fract(sin(n*127.1)*43758.5453); }

void main() {
  float id = aIdx;
  vIdx = id;

  // Stable per-instance position
  float px = rand(id*2.0+0.1)*2.0-1.0;
  float py = rand(id*2.0+0.2)*2.0-1.0;

  // Slow drift
  px += sin(uTime*rand(id*3.0+0.3)*0.4 + id*0.7)*0.04;
  py += cos(uTime*rand(id*3.0+0.4)*0.4 + id*1.3)*0.04;

  // Rotation
  float rot = uTime * uRotSpeed * (rand(id)-0.5)*2.0 + rand(id*5.0)*6.2832;
  float cr = cos(rot), sr = sin(rot);

  // Pulse
  float pulse = 1.0 + sin(uTime*rand(id*7.0+0.7)*3.0+id)*uPulse;
  float scale = uSize * pulse * (0.4 + rand(id*4.0)*0.6);

  // Rotate shape vertex around instance centre
  vec2 rv = vec2(position.x*cr - position.y*sr,
                 position.x*sr + position.y*cr);
  vec2 world = vec2(px, py) + rv * scale;

  // Sample mask at world position
  vec2 maskUv = clamp(world*0.5+0.5, 0.0, 1.0);
  vMask = uHasMask ? texture2D(uMask, maskUv).r : 1.0;

  // Detect shape edge via UV proximity to 0/1
  vec2 uvc = uv;
  float e = min(min(uvc.x, 1.0-uvc.x), min(uvc.y, 1.0-uvc.y));
  vEdge = 1.0 - smoothstep(0.0, 0.12, e);

  gl_Position = vec4(world, 0.0, 1.0);
}
`

const frag = `
precision highp float;

uniform vec3  uColor;
uniform float uOpacity;
uniform float uWireframe;   // 0 solid, 1 wire
uniform float uGlow;
uniform float uTime;

varying float vMask;
varying float vEdge;
varying float vIdx;

float rand(float n){ return fract(sin(n*127.1)*43758.5453); }

void main() {
  if(vMask < 0.04) discard;

  // Per-instance hue tilt
  float h = rand(vIdx) * 0.4;
  vec3 col = mix(uColor, vec3(uColor.g, uColor.b, uColor.r), h);

  float solid = uWireframe > 0.5 ? vEdge : 1.0;
  float glowPulse = uGlow * (0.5 + 0.5*sin(uTime*2.5 + vIdx));

  vec3 finalCol = col*(solid*0.5+0.5) + col*glowPulse;
  float alpha   = (solid*0.7+0.3) * vMask * uOpacity;

  gl_FragColor = vec4(finalCol, alpha);
}
`

// Shape factories return { positions:Float32Array, uvs:Float32Array, indices?:Uint16Array }
const SHAPES = {
  Circle:   (n=32) => new THREE.CircleGeometry(1, n),
  Triangle: () => {
    const g = new THREE.BufferGeometry()
    g.setAttribute('position', new THREE.BufferAttribute(new Float32Array([0,1,0, -0.866,-0.5,0, 0.866,-0.5,0]),3))
    g.setAttribute('uv', new THREE.BufferAttribute(new Float32Array([0.5,1,0,0,1,0]),2))
    return g
  },
  Hexagon:  () => new THREE.CircleGeometry(1, 6),
  Ring:     () => new THREE.RingGeometry(0.55, 1, 32),
  Square:   () => new THREE.PlaneGeometry(2, 2),
  Star: () => {
    const pts=5, verts=[], uvs=[], idx=[]
    for(let i=0;i<pts*2;i++){
      const a=i*Math.PI/pts - Math.PI/2
      const r=i%2===0?1:0.4
      verts.push(Math.cos(a)*r, Math.sin(a)*r, 0)
      uvs.push(Math.cos(a)*0.5+0.5, Math.sin(a)*0.5+0.5)
    }
    for(let i=1;i<pts*2-1;i++) idx.push(0,i,i+1)
    idx.push(0,pts*2-1,1)
    const g=new THREE.BufferGeometry()
    g.setAttribute('position',new THREE.BufferAttribute(new Float32Array(verts),3))
    g.setAttribute('uv',new THREE.BufferAttribute(new Float32Array(uvs),2))
    g.setIndex(idx)
    return g
  },
}

export class GeometryShader {
  constructor() {
    this.params = {
      shape: 'Hexagon', count: 60, size: 0.06,
      rotSpeed: 0.5, pulse: 0.3,
      color: '#44aaff', wireframe: true, glow: 0.5,
    }
    this._buildMesh()
  }

  _buildMesh() {
    const n       = this.params.count
    const baseGeo = SHAPES[this.params.shape]?.() ?? SHAPES.Hexagon()

    const instGeo = new THREE.InstancedBufferGeometry()
    instGeo.index = baseGeo.index
    instGeo.attributes.position = baseGeo.attributes.position
    const rawUv = baseGeo.attributes.uv ?? new THREE.BufferAttribute(new Float32Array(baseGeo.attributes.position.count*2).fill(0.5), 2)
    instGeo.attributes.uv = rawUv

    const idxAttr = new Float32Array(n)
    for(let i=0;i<n;i++) idxAttr[i]=i
    instGeo.setAttribute('aIdx', new THREE.InstancedBufferAttribute(idxAttr, 1))
    instGeo.instanceCount = n

    this.uniforms = {
      uTime:      { value: 0 },
      uOpacity:   { value: 1 },
      uMask:      { value: null },
      uHasMask:   { value: false },
      uN:         { value: n },
      uSize:      { value: this.params.size },
      uRotSpeed:  { value: this.params.rotSpeed },
      uPulse:     { value: this.params.pulse },
      uColor:     { value: new THREE.Color(this.params.color) },
      uWireframe: { value: this.params.wireframe ? 1 : 0 },
      uGlow:      { value: this.params.glow },
    }

    const mat = new THREE.ShaderMaterial({
      vertexShader: vert,
      fragmentShader: frag,
      uniforms: this.uniforms,
      transparent: true,
      depthWrite: false,
      blending: THREE.AdditiveBlending,
      side: THREE.DoubleSide,
    })

    if (this.threeObject) this.threeObject.geometry.dispose()
    const mesh = new THREE.Mesh(instGeo, mat)
    this.threeObject = mesh
    this._instGeo = instGeo
  }

  _rebuild(scene) {
    const old = this.threeObject
    this._buildMesh()
    if (scene) { scene.remove(old); scene.add(this.threeObject) }
  }

  update(time) { this.uniforms.uTime.value = time }

  buildGUI(folder) {
    const p = this.params; const u = this.uniforms
    folder.add(p,'shape', Object.keys(SHAPES)).name('Shape').onChange(() => {
      // Rebuild geometry; needs scene ref — store it
      this._rebuild(this.threeObject?.parent)
    })
    folder.add(p,'count',1,300,1).name('Count').onChange(v => {
      u.uN.value = v; this._rebuild(this.threeObject?.parent)
    })
    folder.add(p,'size',0.005,0.3,0.001).name('Size').onChange(v=>u.uSize.value=v)
    folder.add(p,'rotSpeed',0,5,0.01).name('Rotation Speed').onChange(v=>u.uRotSpeed.value=v)
    folder.add(p,'pulse',0,1,0.01).name('Pulse').onChange(v=>u.uPulse.value=v)
    folder.addColor(p,'color').name('Color').onChange(v=>u.uColor.value.set(v))
    folder.add(p,'wireframe').name('Wireframe').onChange(v=>u.uWireframe.value=v?1:0)
    folder.add(p,'glow',0,2,0.01).name('Glow').onChange(v=>u.uGlow.value=v)
  }
}
