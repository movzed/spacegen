import * as THREE from 'three'
import { MaskSequence } from './MaskSequence.js'

const BLEND_MODES = {
  Additive: () => ({ blending: THREE.AdditiveBlending }),
  Normal:   () => ({ blending: THREE.NormalBlending }),
  Screen:   () => ({
    blending: THREE.CustomBlending,
    blendEquation: THREE.AddEquation,
    blendSrc: THREE.OneFactor,
    blendDst: THREE.OneMinusSrcColorFactor,
  }),
  Multiply: () => ({ blending: THREE.MultiplyBlending }),
}

export class Layer {
  constructor(shaderInst, renderer, name) {
    this.name = name
    this.shaderInst = shaderInst
    this.renderer = renderer
    this.enabled = true
    this.opacity = 1
    this.blendMode = 'Additive'
    this.maskSequence = null
    this.maskParams = { beatsPerLoop: 1 }

    // Each shader exposes .mesh or .points as .threeObject
    this.threeObject = shaderInst.threeObject
    this._applyBlend()
  }

  _applyBlend() {
    const obj = this.threeObject
    if (!obj || !obj.material) return
    const spec = BLEND_MODES[this.blendMode]?.() || BLEND_MODES.Additive()
    Object.assign(obj.material, spec, { depthWrite: false, transparent: true })
    obj.material.needsUpdate = true
  }

  setOpacity(v) {
    this.opacity = v
    if (this.shaderInst.uniforms?.uOpacity) {
      this.shaderInst.uniforms.uOpacity.value = v
    }
  }

  setBlendMode(mode) {
    this.blendMode = mode
    this._applyBlend()
  }

  async loadMaskSequence(files) {
    if (this.maskSequence) this.maskSequence.dispose()
    this.maskSequence = new MaskSequence(files)
    await this.maskSequence.load()
    const u = this.shaderInst.uniforms
    if (u?.uMask) u.uMask.value = this.maskSequence.currentTexture()
    if (u?.uHasMask) u.uHasMask.value = true
  }

  update(time, delta, beat, bpmClock) {
    if (!this.enabled) { this.threeObject.visible = false; return }
    this.threeObject.visible = true

    if (this.maskSequence && bpmClock.bpm > 0) {
      this.maskSequence.update(delta, bpmClock.bpm, this.maskParams.beatsPerLoop)
      const u = this.shaderInst.uniforms
      if (u?.uMask) u.uMask.value = this.maskSequence.currentTexture()
    }

    this.shaderInst.update?.(time, delta, beat, bpmClock)
  }

  onResize(w, h) {
    const u = this.shaderInst.uniforms
    if (u?.uResolution) u.uResolution.value.set(w, h)
    this.shaderInst.onResize?.(w, h)
  }

  buildGUI(folder) {
    this.shaderInst.buildGUI?.(folder)
  }
}

export class LayerManager {
  constructor() {
    this.layers = []
    this._counter = 0
  }

  get count() { return this.layers.length }

  add(shaderInst, renderer, name) {
    const layer = new Layer(shaderInst, renderer, name || `Layer ${++this._counter}`)
    this.layers.push(layer)
    return layer
  }

  remove(layer, scene) {
    scene.remove(layer.threeObject)
    this.layers = this.layers.filter(l => l !== layer)
  }

  update(time, delta, beat, bpmClock) {
    for (const layer of this.layers) layer.update(time, delta, beat, bpmClock)
  }

  onResize(w, h) {
    for (const layer of this.layers) layer.onResize(w, h)
  }
}
