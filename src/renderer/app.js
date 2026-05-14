import * as THREE from 'three'
import { GUI } from 'lil-gui'
import { LayerManager } from './LayerManager.js'
import { BPMClock } from './BPMClock.js'
import { ChaserShader } from './shaders/ChaserShader.js'
import { SaberShader } from './shaders/SaberShader.js'
import { ParticleShader } from './shaders/ParticleShader.js'
import { GeometryShader } from './shaders/GeometryShader.js'
import { KaleidoscopeShader } from './shaders/KaleidoscopeShader.js'
import { VoronoiShader } from './shaders/VoronoiShader.js'
import { ChromaGlitchShader } from './shaders/ChromaGlitchShader.js'
import { PlasmaWaveShader } from './shaders/PlasmaWaveShader.js'
import { StarFieldShader } from './shaders/StarFieldShader.js'
import { DomainWarpShader } from './shaders/DomainWarpShader.js'

const { ipcRenderer } = require('electron')

const SHADER_REGISTRY = {
  Chaser:        renderer => new ChaserShader(renderer),
  Saber:         renderer => new SaberShader(renderer),
  Particles:     renderer => new ParticleShader(renderer),
  Geometry:      renderer => new GeometryShader(renderer),
  Kaleidoscope:  renderer => new KaleidoscopeShader(renderer),
  Voronoi:       renderer => new VoronoiShader(renderer),
  ChromaGlitch:  renderer => new ChromaGlitchShader(renderer),
  PlasmaWave:    renderer => new PlasmaWaveShader(renderer),
  StarField:     renderer => new StarFieldShader(renderer),
  DomainWarp:    renderer => new DomainWarpShader(renderer),
}

class SpaceGen {
  constructor() {
    this.clock   = new THREE.Clock()
    this.bpm     = new BPMClock()
    this.layers  = new LayerManager()
    this._fpsFrames = 0
    this._fpsAccum  = 0
    this._fpsEl = document.getElementById('fps')
    this._bpmEl = document.getElementById('bpm-display')

    this._initRenderer()
    this._initGUI()
    this._animate()
  }

  _initRenderer() {
    this.renderer = new THREE.WebGLRenderer({ antialias: false, powerPreference: 'high-performance' })
    this.renderer.setPixelRatio(window.devicePixelRatio)
    this.renderer.setSize(window.innerWidth, window.innerHeight)
    this.renderer.setClearColor(0x000000, 1)
    document.getElementById('canvas-container').appendChild(this.renderer.domElement)

    this.camera = new THREE.OrthographicCamera(-1, 1, 1, -1, 0, 1)
    this.scene  = new THREE.Scene()

    window.addEventListener('resize', () => {
      const w = window.innerWidth, h = window.innerHeight
      this.renderer.setSize(w, h)
      this.layers.onResize(w, h)
    })
  }

  _initGUI() {
    this.gui = new GUI({ title: 'SpaceGen' })

    // BPM
    const bpmFolder = this.gui.addFolder('⏱ BPM Clock')
    const bpmProxy  = { bpm: 120, tap: () => this.bpm.tap() }
    bpmFolder.add(bpmProxy, 'bpm', 0, 300, 0.1).name('BPM').onChange(v => {
      this.bpm.setBPM(v)
      if (this._bpmEl) this._bpmEl.textContent = v.toFixed(1)
    })
    bpmFolder.add(bpmProxy, 'tap').name('Tap Tempo')
    bpmFolder.open()

    // Add layer controls
    const addFolder = this.gui.addFolder('＋ Add Layer')
    Object.keys(SHADER_REGISTRY).forEach(name => {
      addFolder.add({ add: () => this._addLayer(name) }, 'add').name(name)
    })
    addFolder.open()

    this._layerFolder = this.gui.addFolder('Layers')
    this._layerFolder.open()
  }

  _addLayer(shaderName) {
    const shaderInst = SHADER_REGISTRY[shaderName](this.renderer)
    const layer = this.layers.add(shaderInst, this.renderer, shaderName)
    this.scene.add(layer.threeObject)

    const f = this._layerFolder.addFolder(`▪ ${shaderName} #${this.layers.count}`)

    f.add(layer, 'enabled').name('Enabled')
    f.add({ opacity: 1 }, 'opacity', 0, 1, 0.01).name('Opacity').onChange(v => layer.setOpacity(v))
    f.add({ blend: 'Additive' }, 'blend', ['Additive','Normal','Screen','Multiply']).name('Blend').onChange(v => layer.setBlendMode(v))

    // Mask loader
    f.add({
      load: async () => {
        const dir = await ipcRenderer.invoke('dialog:openDirectory')
        if (!dir) return
        const files = await ipcRenderer.invoke('fs:readDir', dir)
        if (!files.length) { alert('No PNG/JPG files found in that folder.'); return }
        await layer.loadMaskSequence(files)
        // After load, add beats/loop control if not already there
        if (!layer._beatsCtrl) {
          layer._beatsCtrl = f.add(layer.maskParams, 'beatsPerLoop', [0.25,0.5,1,2,4,8]).name('Beats / Loop')
        }
      }
    }, 'load').name('Load Mask Sequence…')

    // Remove button
    f.add({ remove: () => { this.layers.remove(layer, this.scene); f.destroy() } }, 'remove').name('Remove Layer')

    layer.buildGUI(f)
    f.open()
  }

  _animate() {
    requestAnimationFrame(() => this._animate())

    const delta   = this.clock.getDelta()
    const elapsed = this.clock.getElapsedTime()
    const beat    = this.bpm.update(delta)

    // FPS display
    this._fpsAccum += delta; this._fpsFrames++
    if (this._fpsAccum >= 0.5) {
      if (this._fpsEl) this._fpsEl.textContent = Math.round(this._fpsFrames / this._fpsAccum)
      this._fpsFrames = 0; this._fpsAccum = 0
    }

    this.layers.update(elapsed, delta, beat, this.bpm)
    this.renderer.render(this.scene, this.camera)
  }
}

window.addEventListener('DOMContentLoaded', () => new SpaceGen())
