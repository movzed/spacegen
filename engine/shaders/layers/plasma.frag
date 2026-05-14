#version 410 core

// ─────────────────────────────────────────────────────────────────────────────
// plasma.frag — PlasmaWave layer
//
// Four overlapping sine waves in UV space → cosine palette colour mapping.
// BPM-reactive: beat flash boosts frequency momentarily.
// ─────────────────────────────────────────────────────────────────────────────

in  vec2 vUV;
out vec4 fragColor;

uniform vec2  uResolution;
uniform float uTime;
uniform float uOpacity;
uniform float uSpeed;
uniform float uScale;
uniform float uContrast;
uniform float uBeatFlash;

uniform vec3 uColor1;
uniform vec3 uColor2;
uniform vec3 uColor3;

// Mask
uniform sampler2D uMask;
uniform bool      uHasMask;

// ── Cosine palette (Inigo Quilez) ─────────────────────────────────────────────
vec3 palette(float t, vec3 a, vec3 b, vec3 c, vec3 d) {
    return a + b * cos(6.28318 * (c * t + d));
}

// ── Plasma field ──────────────────────────────────────────────────────────────
float plasmaField(vec2 p, float t) {
    float flash = 1.0 + uBeatFlash * 2.0;
    float s1 = sin((p.x * flash + t) * uScale);
    float s2 = sin((p.y * flash + t * 0.7) * uScale);
    float s3 = sin(((p.x + p.y) + t * 0.6) * uScale * 0.8);
    float s4 = sin((length(p - 0.5) * uScale * 2.0) - t * 1.2);
    return (s1 + s2 + s3 + s4) * 0.25;
}

void main() {
    // Aspect-correct UV
    vec2 uv = vUV;
    uv.x   *= uResolution.x / uResolution.y;

    float t = uTime * uSpeed;
    float v = plasmaField(uv, t);

    // Map to [0,1]
    v = v * 0.5 + 0.5;
    v = pow(clamp(v, 0.0, 1.0), uContrast);

    // Tri-tone palette blend
    vec3 col;
    if (v < 0.5)
        col = mix(uColor1, uColor2, v * 2.0);
    else
        col = mix(uColor2, uColor3, (v - 0.5) * 2.0);

    // HDR boost on beat
    col *= 1.0 + uBeatFlash * 0.5;

    // Mask
    float maskAlpha = 1.0;
    if (uHasMask) maskAlpha = texture(uMask, vUV).r;

    fragColor = vec4(col * maskAlpha, maskAlpha * uOpacity);
}
