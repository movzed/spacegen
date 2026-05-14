#pragma once
#include <chrono>
#include <array>

// High-precision BPM clock using std::chrono.
// Outputs:
//   beat     — true for exactly one frame when a beat boundary is crossed
//   phase    — 0..1 sawtooth within the current beat
//   elapsed  — total seconds since start
class BPMClock {
public:
    BPMClock();

    // Call once per frame. Returns true on a beat boundary.
    bool update();

    // Tap tempo: call on each user tap. Averages last 8 intervals.
    void tap();

    void  setBPM(double bpm);
    double bpm()     const { return m_bpm; }
    double phase()   const { return m_phase; }    // 0..1
    double elapsed() const { return m_elapsed; }  // seconds
    float  delta()   const { return m_delta; }    // seconds since last frame
    int    beatCount()const{ return m_beatCount; }

private:
    using Clock    = std::chrono::steady_clock;
    using TimePoint= std::chrono::time_point<Clock>;

    double    m_bpm      = 120.0;
    double    m_elapsed  = 0.0;
    double    m_phase    = 0.0;
    float     m_delta    = 0.0f;
    int       m_beatCount= 0;

    TimePoint m_startTime;
    TimePoint m_lastFrame;

    // Tap tempo ring buffer
    std::array<TimePoint, 8> m_taps{};
    int  m_tapCount = 0;
    int  m_tapHead  = 0;
};
