#pragma once

struct MasterFX {
    float brightness = 0.0f;   // -1 .. +1
    float contrast   = 1.0f;   //  0 .. 3
    float saturation = 1.0f;   //  0 .. 3
    float hue        = 0.0f;   //  0 .. 1  (full colour circle)
    float glow       = 0.0f;   //  0 .. 1
    float vignette   = 0.0f;   //  0 .. 1
    float feedback   = 0.0f;   //  0 .. 1
    float aberration = 0.0f;   //  0 .. 1
};
