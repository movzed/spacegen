// noise.glsl — shared noise functions (include via #include in layers)
// Usage: #include "common/noise.glsl"  (shader copy target puts both in same dir)

// ── Hash ──────────────────────────────────────────────────────────────────────

float hash11(float n) { return fract(sin(n) * 43758.5453123); }
float hash12(vec2  p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
vec2  hash22(vec2  p) {
    p = vec2(dot(p, vec2(127.1, 311.7)), dot(p, vec2(269.5, 183.3)));
    return fract(sin(p) * 43758.5453);
}
vec3  hash33(vec3  p) {
    p = vec3(dot(p, vec3(127.1,311.7,74.7)),
             dot(p, vec3(269.5,183.3,246.1)),
             dot(p, vec3(113.5,271.9,124.6)));
    return fract(sin(p) * 43758.5453);
}

// ── Value noise ───────────────────────────────────────────────────────────────

float vnoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = hash12(i);
    float b = hash12(i + vec2(1.0, 0.0));
    float c = hash12(i + vec2(0.0, 1.0));
    float d = hash12(i + vec2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

// ── Fractal Brownian Motion (fBm) ─────────────────────────────────────────────

float fbm(vec2 p, int octaves) {
    float v = 0.0, a = 0.5;
    mat2  rot = mat2(cos(0.5), sin(0.5), -sin(0.5), cos(0.5));
    for (int i = 0; i < octaves; ++i) {
        v += a * vnoise(p);
        p  = rot * p * 2.0;
        a *= 0.5;
    }
    return v;
}

// ── Worley (cellular) noise ───────────────────────────────────────────────────

float worley(vec2 p) {
    vec2  ip = floor(p);
    float d  = 1e9;
    for (int j = -1; j <= 1; ++j)
    for (int i = -1; i <= 1; ++i) {
        vec2 cell = ip + vec2(i, j);
        vec2 o    = hash22(cell);
        float dist = length(fract(p) - (vec2(i, j) + o));
        d = min(d, dist);
    }
    return d;
}

// ── IQ domain warp ───────────────────────────────────────────────────────────

vec2 domainWarp(vec2 p, float strength) {
    float ox = fbm(p + vec2(0.0,  0.0), 4);
    float oy = fbm(p + vec2(5.2,  1.3), 4);
    return p + strength * vec2(ox, oy);
}
