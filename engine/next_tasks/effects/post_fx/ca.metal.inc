// ca.metal.inc — MSL fragment shader for ChromaticAberrationEffect.
// Included as a C++ raw-string literal from ChromaticAberrationEffect.cpp.
//
// Single fullscreen-quad pass. VS emits clip-space corners + UV from a
// vertex_id-based table — no vertex buffer needed.

R"MSL(
#include <metal_stdlib>
using namespace metal;

struct CaUniforms {
    // .x = strength (0..0.1)
    // .y = center.x (0..1)
    // .z = center.y (0..1)
    // .w = pattern  (int cast to float: 0=radial 1=linear 2=barrel 3=pincushion)
    float4 params;
};

struct VsOut {
    float4 position [[position]];
    float2 uv;
};

vertex VsOut ca_vs(uint vid [[vertex_id]]) {
    // Fullscreen triangle-strip: 4 verts → 2 tris covering NDC [-1,1]².
    float2 corners[4] = {
        float2(-1.0, -1.0),
        float2( 1.0, -1.0),
        float2(-1.0,  1.0),
        float2( 1.0,  1.0),
    };
    float2 c = corners[vid];
    VsOut o;
    o.position = float4(c, 0.0, 1.0);
    // Flip Y so uv.y=0 is the top of the screen (matches Metal texture
    // convention for our offscreen color target).
    o.uv = float2(c.x * 0.5 + 0.5, 1.0 - (c.y * 0.5 + 0.5));
    return o;
}

fragment float4 ca_fs(VsOut in                            [[stage_in]],
                       constant CaUniforms& u             [[buffer(0)]],
                       texture2d<float, access::sample> src [[texture(0)]],
                       sampler                          smp [[sampler(0)]])
{
    float  strength = u.params.x;
    float2 center   = u.params.yz;
    int    pattern  = int(u.params.w + 0.5);

    // Bypass: at strength 0 just pass through. Saves the 3 extra samples
    // when the effect is in the bus but slammed to 0 (modulator bank etc).
    if (strength <= 1e-5) {
        return src.sample(smp, in.uv);
    }

    float2 v    = in.uv - center;
    float  dist = length(v);

    // Per-pattern offset shape. `dir` is the direction we push channels
    // along (and `scale` controls how much).
    float2 dir;
    float  scale;
    switch (pattern) {
        case 1: { // Linear (horizontal smear)
            dir   = float2(1.0, 0.0);
            scale = strength;
            break;
        }
        case 2: { // Barrel (positive distortion: ~ dist^2)
            dir   = v;
            scale = strength * (1.0 + dist * dist * 4.0);
            break;
        }
        case 3: { // Pincushion (negative distortion)
            dir   = v;
            // Saturate to avoid sign flip near center.
            scale = strength * max(1.0 - dist * dist * 4.0, 0.0);
            // Pull channels INWARD for pincushion → negate offset.
            scale = -scale;
            break;
        }
        default: { // 0 = Radial
            dir   = v;
            scale = strength * dist;
            break;
        }
    }

    // Three samples — red outside, green at base, blue inside.
    // For Linear / Barrel / Pincushion the `dir` is shared but each
    // channel still gets a different magnitude on the same direction.
    float2 uvR = in.uv + dir * (+scale);
    float2 uvG = in.uv;
    float2 uvB = in.uv + dir * (-scale);

    float4 cR = src.sample(smp, uvR);
    float4 cG = src.sample(smp, uvG);
    float4 cB = src.sample(smp, uvB);

    // Compose: take R from the outer sample, G from the center, B from
    // the inner sample. Alpha tracks the green sample (it's the "core").
    return float4(cR.r, cG.g, cB.b, cG.a);
}
)MSL"
