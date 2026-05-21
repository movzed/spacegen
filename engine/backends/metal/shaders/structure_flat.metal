// Flat-color structure shader. M2-B placeholder before PBR-light (M2-C).
// Draws the structure mesh through the camera matrices; alpha is solid.

#include <metal_stdlib>
using namespace metal;

struct VertexIn {
    float3 position [[attribute(0)]];
};

struct VertexOut {
    float4 position [[position]];
    float3 worldPos;
};

struct Uniforms {
    float4x4 projection;   // GL->Metal converted in C++ before upload
    float4x4 view;
    float4x4 model;
    float4   tintColor;
};

vertex VertexOut vs_main(VertexIn in [[stage_in]],
                         constant Uniforms& u [[buffer(1)]])
{
    VertexOut out;
    float4 world = u.model * float4(in.position, 1.0);
    out.position = u.projection * u.view * world;
    out.worldPos = world.xyz;
    return out;
}

fragment float4 fs_main(VertexOut in [[stage_in]],
                        constant Uniforms& u [[buffer(0)]])
{
    return u.tintColor;
}
