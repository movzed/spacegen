import * as THREE from 'three'

export class MaskSequence {
  constructor(filePaths) {
    this.filePaths = filePaths
    this.textures = []
    this.currentFrame = 0
    this.frameAccum = 0
    this.loaded = false
  }

  async load() {
    const loader = new THREE.TextureLoader()
    this.textures = await Promise.all(
      this.filePaths.map(fp =>
        loader.loadAsync('file://' + fp).then(t => {
          t.minFilter = THREE.LinearFilter
          t.magFilter = THREE.LinearFilter
          t.wrapS = t.wrapT = THREE.RepeatWrapping
          return t
        })
      )
    )
    this.loaded = true
  }

  // beatsPerLoop: how many BPM beats = one full sequence loop
  update(delta, bpm, beatsPerLoop) {
    if (!this.loaded || this.textures.length === 0 || bpm === 0) return
    const loopsPerSec = bpm / (60 * (beatsPerLoop || 1))
    this.frameAccum += delta * loopsPerSec * this.textures.length
    while (this.frameAccum >= 1) {
      this.currentFrame = (this.currentFrame + 1) % this.textures.length
      this.frameAccum -= 1
    }
  }

  currentTexture() {
    return this.textures[this.currentFrame] || null
  }

  dispose() {
    this.textures.forEach(t => t.dispose())
    this.textures = []
    this.loaded = false
  }
}
