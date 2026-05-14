#include "BPMClock.h"
#include <cmath>
#include <algorithm>

BPMClock::BPMClock() {
    m_startTime = Clock::now();
    m_lastFrame = m_startTime;
}

bool BPMClock::update() {
    auto  now     = Clock::now();
    m_delta       = std::chrono::duration<float>(now - m_lastFrame).count();
    m_elapsed    += static_cast<double>(m_delta);
    m_lastFrame   = now;

    if (m_bpm <= 0.0) { m_phase = 0.0; return false; }

    double beatDur = 60.0 / m_bpm;
    double prevPhase = m_phase;

    // Advance phase continuously — no accumulated drift
    m_phase = std::fmod(m_elapsed / beatDur, 1.0);

    bool beat = m_phase < prevPhase;  // wrapped around 0
    if (beat) ++m_beatCount;
    return beat;
}

void BPMClock::setBPM(double bpm) {
    // Preserve beat phase continuity when BPM changes
    if (m_bpm > 0.0 && bpm > 0.0) {
        double oldBeatDur = 60.0 / m_bpm;
        double newBeatDur = 60.0 / bpm;
        // Keep current phase position
        m_elapsed = m_phase * newBeatDur + std::floor(m_elapsed / oldBeatDur) * newBeatDur;
    }
    m_bpm = std::max(0.0, std::min(300.0, bpm));
}

void BPMClock::tap() {
    auto now = Clock::now();
    m_taps[m_tapHead] = now;
    m_tapHead  = (m_tapHead + 1) % static_cast<int>(m_taps.size());
    m_tapCount = std::min(m_tapCount + 1, static_cast<int>(m_taps.size()));

    if (m_tapCount < 2) return;

    // Average the last N intervals
    double sum = 0.0;
    int    n   = m_tapCount - 1;
    for (int i = 0; i < n; ++i) {
        int a = (m_tapHead - 2 - i + m_taps.size()) % m_taps.size();
        int b = (m_tapHead - 1 - i + m_taps.size()) % m_taps.size();
        sum  += std::chrono::duration<double>(m_taps[b] - m_taps[a]).count();
    }
    double avgInterval = sum / static_cast<double>(n);
    setBPM(60.0 / avgInterval);

    // Re-sync phase to the latest tap
    m_elapsed = 0.0;
    m_phase   = 0.0;
}
