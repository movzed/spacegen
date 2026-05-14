import * as THREE from 'three'

// CPU particle system. ~5k particles easily real-time.
// Particles spawn across the canvas; mask restricts their visibility via the shader.
const vert = `
attribute float aLife;
attribute float aSeed;
attribute vec3  aColor;
uniform float uSize;

varying float vLife;
varying vec3  vColor;

void main() {
  vLife  = aLife;
  vColor = aColor;
  vec4 mvPos = modelViewMatrix * vec4(position, 1.0);
  gl_PointSize = uSize * aLife * (300.0 / -mvPos.z);
  gl_Position  = projectionMatrix * mvPos;
}
`

const frag = `
precision highp float;
uniform float uOpacity;
uniform sampler2D uMask;
uniform bool uHasMask;

varying float vLife;
varying vec3  vColor;

void main() {
  // Soft circle sprite
  vec2 coord = gl_PointCoord - 0.5;
  float r = dot(coord, coord);
  if(r > 0.25) discard;
  float alpha = (1.0 - r*4.0) * vLife * uOpacity;
  gl_FragColor = vec4(vColor, alpha);
}
`

export class ParticleShader {
  constructor() {
    this.MAX = 5000
    this._count = 2000

    // Per-particle state (CPU)
    this._pos  = new Float32Array(this.MAX * 3)
    this._vel  = new Float32Array(this.MAX * 2)
    this._life = new Float32Array(this.MAX)     // 0–1
    this._maxL = new Float32Array(this.MAX)
    this._seed = new Float32Array(this.MAX)

    // GPU attributes
    this._aPos   = new THREE.BufferAttribute(new Float32Array(this.MAX * 3), 3).setUsage(THREE.DynamicDrawUsage)
    this._aLife  = new THREE.BufferAttribute(new Float32Array(this.MAX), 1).setUsage(THREE.DynamicDrawUsage)
    this._aSeed  = new THREE.BufferAttribute(new Float32Array(this.MAX), 1)
    this._aColor = new THREE.BufferAttribute(new Float32Array(this.MAX * 3), 3).setUsage(THREE.DynamicDrawUsage)

    const geo = new THREE.BufferGeometry()
    geo.setAttribute('position', this._aPos)
    geo.setAttribute('aLife',    this._aLife)
    geo.setAttribute('aSeed',    this._aSeed)
    geo.setAttribute('aColor',   this._aColor)
    geo.setDrawRange(0, this._count)

    this.uniforms = {
      uSize:    { value: 4 },
      uOpacity: { value: 1 },
      uMask:    { value: null },
      uHasMask: { value: false },
    }

    const mat = new THREE.ShaderMaterial({
      vertexShader: vert,
      fragmentShader: frag,
      uniforms: this.uniforms,
      transparent: true,
      depthWrite: false,
      blending: THREE.AdditiveBlending,
      vertexColors: true,
    })

    this.threeObject = new THREE.Points(geo, mat)
    this._geo = geo

    // Params
    this.params = {
      count:         2000,
      size:          4,
      speed:         0.25,
      gravity:       -0.03,
      lifetime:      3.0,
      spread:        0.08,
      turbulence:    0.04,
      color:         '#88aaff',
      colorVariance: 0.3,
    }

    this._baseColor = new THREE.Color(this.params.color)
    this._initAll()
  }

  _initAll() {
    for (let i = 0; i < this.MAX; i++) this._spawn(i, true)
    this._flush()
  }

  _spawn(i, randomLife = false) {
    this._pos[i*3]   = Math.random() * 2 - 1
    this._pos[i*3+1] = Math.random() * 2 - 1
    this._pos[i*3+2] = 0

    const angle = Math.random() * Math.PI * 2
    const sp    = (0.5 + Math.random()*0.5) * this.params.spread
    this._vel[i*2]   = Math.cos(angle) * sp
    this._vel[i*2+1] = Math.sin(angle) * sp

    const ml = this.params.lifetime * (0.5 + Math.random()*0.5)
    this._maxL[i] = ml
    this._life[i] = randomLife ? Math.random() * ml : ml
    this._seed[i] = Math.random()

    const v = 1 - Math.random() * this.params.colorVariance
    this._aColor.array[i*3]   = this._baseColor.r * v
    this._aColor.array[i*3+1] = this._baseColor.g * v
    this._aColor.array[i*3+2] = this._baseColor.b * v
  }

  _flush() {
    this._aPos.array.set(this._pos)
    this._aLife.array.set(this._life.map((l,i) => l / this._maxL[i]))
    this._aSeed.array.set(this._seed)
    this._aPos.needsUpdate   = true
    this._aLife.needsUpdate  = true
    this._aColor.needsUpdate = true
  }

  update(time, delta) {
    const n    = this._count
    const grav = this.params.gravity
    const sp   = this.params.speed
    const turb = this.params.turbulence

    for (let i = 0; i < n; i++) {
      this._life[i] -= delta

      if (this._life[i] <= 0) {
        this._spawn(i, false)
        continue
      }

      // Turbulence
      const tx = Math.sin(time*2.3 + i*0.17) * Math.cos(time*1.7 + i*0.31) * turb
      const ty = Math.cos(time*1.9 + i*0.23) * Math.sin(time*2.1 + i*0.11) * turb

      this._vel[i*2]   += tx * delta
      this._vel[i*2+1] += (ty + grav) * delta

      let x = this._pos[i*3]   + this._vel[i*2]   * delta * sp
      let y = this._pos[i*3+1] + this._vel[i*2+1] * delta * sp

      // Wrap
      if (x >  1) x -= 2; if (x < -1) x += 2
      if (y >  1) y -= 2; if (y < -1) y += 2

      this._pos[i*3] = x; this._pos[i*3+1] = y

      const norm = this._life[i] / this._maxL[i]
      this._aLife.array[i] = norm

      const fade = Math.min(norm * 3, 1)
      this._aColor.array[i*3]   = this._baseColor.r * fade
      this._aColor.array[i*3+1] = this._baseColor.g * fade
      this._aColor.array[i*3+2] = this._baseColor.b * fade
    }

    this._aPos.array.set(this._pos.subarray(0, n*3))
    this._aPos.needsUpdate  = true
    this._aLife.needsUpdate = true
    this._aColor.needsUpdate = true
  }

  buildGUI(folder) {
    const p = this.params
    const u = this.uniforms

    folder.add(p,'count',100,this.MAX,100).name('Count').onChange(v => {
      this._count = v; this._geo.setDrawRange(0, v)
    })
    folder.add(p,'size',1,20,0.5).name('Size').onChange(v=>u.uSize.value=v)
    folder.add(p,'speed',0,3,0.01).name('Speed')
    folder.add(p,'gravity',-0.5,0.5,0.001).name('Gravity')
    folder.add(p,'lifetime',0.1,10,0.1).name('Lifetime (s)')
    folder.add(p,'spread',0,0.5,0.001).name('Spread')
    folder.add(p,'turbulence',0,0.3,0.001).name('Turbulence')
    folder.addColor(p,'color').name('Color').onChange(v => {
      this._baseColor.set(v)
      for (let i=0; i<this.MAX; i++) this._spawn(i, true)
    })
    folder.add(p,'colorVariance',0,1,0.01).name('Color Variance')
  }
}
