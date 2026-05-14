export class BPMClock {
  constructor() {
    this.bpm = 120
    this.elapsed = 0
    this.phase = 0
    this.beatCount = 0
    this._taps = []
  }

  get beatDuration() {
    return this.bpm > 0 ? 60 / this.bpm : Infinity
  }

  setBPM(v) {
    this.bpm = Math.max(0, Math.min(300, v))
  }

  tap() {
    const now = performance.now()
    this._taps.push(now)
    if (this._taps.length > 8) this._taps.shift()
    if (this._taps.length >= 2) {
      let sum = 0
      for (let i = 1; i < this._taps.length; i++) sum += this._taps[i] - this._taps[i - 1]
      this.setBPM(60000 / (sum / (this._taps.length - 1)))
    }
  }

  // Returns true on a beat boundary
  update(delta) {
    if (this.bpm === 0) return false
    this.elapsed += delta
    const prev = this.phase
    this.phase = (this.elapsed / this.beatDuration) % 1
    const fired = this.phase < prev
    if (fired) this.beatCount++
    return fired
  }
}
