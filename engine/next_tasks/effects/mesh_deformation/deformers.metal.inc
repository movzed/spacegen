// deformers.metal.inc
// =====================
// MSL snippet — paste verbatim into kStructurePbrMSL (MetalRenderer.cpp)
// right before vs_main, then replace the existing displace + twist block
// inside vs_main with a single call to `applyDeformerChain(pos, normal,
// u.deformers, u.deformCount, u.ffdCorners)`.
//
// One self-contained file so the algorithm stays close to the math —
// see ../README.md for derivations and references.

// ----------------------------------------------------------------------------
// Constants — keep in sync with MeshDeformationLayer.h.
// ----------------------------------------------------------------------------
constant int OP_NONE        = 0;
constant int OP_CURL_NOISE  = 1;
constant int OP_VORONOI     = 2;
constant int OP_TWIST       = 3;
constant int OP_BEND        = 4;
constant int OP_TAPER       = 5;
constant int OP_WAVE        = 6;
constant int OP_SPHERIFY    = 7;
constant int OP_FFD_LITE    = 8;

constant int MAX_DEFORM_OPS  = 16;
constant int MAX_FFD_SLOTS   = 4;

// One operator on the GPU.
struct GpuDeformOp {
    float4 header;   // .x type, .y intensity, .z time, .w ffd slot index (-1 unless FFD)
    float4 pA;
    float4 pB;
};

// ----------------------------------------------------------------------------
// Hash + noise primitives.
// ----------------------------------------------------------------------------
static float hash11(float n) {
    return fract(sin(n) * 43758.5453123);
}

static float hash13(float3 p) {
    float n = dot(p, float3(127.1, 311.7, 74.7));
    return fract(sin(n) * 43758.5453123) * 2.0 - 1.0;
}

static float3 hash33(float3 p) {
    p = float3(dot(p, float3(127.1, 311.7, 74.7)),
                dot(p, float3(269.5, 183.3, 246.1)),
                dot(p, float3(113.5,  271.9, 124.6)));
    return fract(sin(p) * 43758.5453123) * 2.0 - 1.0;
}

// Quintic Hermite fade for gradient noise (Perlin 2002 improved noise).
static float3 fade5(float3 t) {
    return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
}

// Gradient noise in [-1, 1]. Pattern after Inigo Quílez's analytic
// gradient noise — we don't need analytic gradients here because the
// curl-noise op finite-differences anyway.
static float noise3(float3 p) {
    float3 i = floor(p);
    float3 f = fract(p);
    float3 u = fade5(f);
    float n000 = dot(hash33(i + float3(0,0,0)), f - float3(0,0,0));
    float n100 = dot(hash33(i + float3(1,0,0)), f - float3(1,0,0));
    float n010 = dot(hash33(i + float3(0,1,0)), f - float3(0,1,0));
    float n110 = dot(hash33(i + float3(1,1,0)), f - float3(1,1,0));
    float n001 = dot(hash33(i + float3(0,0,1)), f - float3(0,0,1));
    float n101 = dot(hash33(i + float3(1,0,1)), f - float3(1,0,1));
    float n011 = dot(hash33(i + float3(0,1,1)), f - float3(0,1,1));
    float n111 = dot(hash33(i + float3(1,1,1)), f - float3(1,1,1));
    float nx00 = mix(n000, n100, u.x);
    float nx10 = mix(n010, n110, u.x);
    float nx01 = mix(n001, n101, u.x);
    float nx11 = mix(n011, n111, u.x);
    float nxy0 = mix(nx00, nx10, u.y);
    float nxy1 = mix(nx01, nx11, u.y);
    return mix(nxy0, nxy1, u.z);
}

// Three orthogonal noise samples with disjoint offsets — gives a
// smooth vector field whose curl is divergence-free.
static float3 noiseField(float3 p) {
    return float3(noise3(p + float3(0.0,  0.0,  0.0)),
                   noise3(p + float3(31.4, 17.3, 11.0)),
                   noise3(p + float3(91.0, 64.7, 47.2)));
}

// Rodrigues rotation of vector v around unit axis a by angle theta.
static float3 rotateAxis(float3 v, float3 a, float theta) {
    float c = cos(theta);
    float s = sin(theta);
    return v * c + cross(a, v) * s + a * dot(a, v) * (1.0 - c);
}

// ----------------------------------------------------------------------------
// Deformer 1: Curl Noise displacement.
// pA: (scale, amount, speed, timeEvolveEnable)
//
// curl(F) where F = noiseField. Estimated via finite differences.
// Displacement = curl × amount × intensity.
// ----------------------------------------------------------------------------
static float3 curlOf(float3 p) {
    const float e = 1e-2;
    float3 dx = (noiseField(p + float3(e,0,0)) - noiseField(p - float3(e,0,0))) / (2.0 * e);
    float3 dy = (noiseField(p + float3(0,e,0)) - noiseField(p - float3(0,e,0))) / (2.0 * e);
    float3 dz = (noiseField(p + float3(0,0,e)) - noiseField(p - float3(0,0,e))) / (2.0 * e);
    return float3(dy.z - dz.y, dz.x - dx.z, dx.y - dy.x);
}

static float3 opCurlNoise(float3 pos, GpuDeformOp op) {
    float  scale  = op.pA.x;
    float  amount = op.pA.y;
    float  speed  = op.pA.z;
    bool   timeOn = op.pA.w > 0.5;
    float  t      = op.header.z;
    float3 q      = pos * scale;
    if (timeOn) {
        q += t * speed * float3(1.0, 1.3, 0.7);
    }
    float3 c = curlOf(q);
    return pos + c * amount * op.header.y;
}

// ----------------------------------------------------------------------------
// Deformer 2: Voronoi displacement.
// pA: (scale, shatter, jitter, _unused)
//
// Inigo Quílez's iquilezles.org/articles/voronoilines/ — adapted for
// 3D. We search the 27 cells around floor(p / scale).
// ----------------------------------------------------------------------------
static float3 opVoronoi(float3 pos, GpuDeformOp op) {
    float scale   = max(op.pA.x, 1e-3);
    float shatter = op.pA.y;
    float jitter  = clamp(op.pA.z, 0.0, 1.0);

    float3 q  = pos / scale;
    float3 i  = floor(q);
    float3 f  = q - i;

    float  bestDist = 1e9;
    float3 bestCell = float3(0.0);
    for (int dz = -1; dz <= 1; ++dz)
    for (int dy = -1; dy <= 1; ++dy)
    for (int dx = -1; dx <= 1; ++dx) {
        float3 cell = float3(float(dx), float(dy), float(dz));
        float3 r = (hash33(i + cell) * 0.5 + 0.5) * jitter;
        float3 cellPt = cell + r;
        float3 diff   = cellPt - f;
        float  d      = dot(diff, diff);
        if (d < bestDist) {
            bestDist = d;
            bestCell = cellPt;
        }
    }
    float3 centerWorld = (i + bestCell) * scale;
    float3 toCenter    = centerWorld - pos;
    return pos + toCenter * shatter * op.header.y;
}

// ----------------------------------------------------------------------------
// Deformer 3: Twist around axis.
// pA: (axis.xyz, amount-rad-per-m)
// pB: (center.xyz, _unused)
// ----------------------------------------------------------------------------
static float3 opTwist(float3 pos, GpuDeformOp op) {
    float3 axis   = normalize(op.pA.xyz + float3(1e-6));
    float  amount = op.pA.w * op.header.y;
    float3 center = op.pB.xyz;
    float3 q      = pos - center;
    float  h      = dot(q, axis);
    float  theta  = h * amount;
    return rotateAxis(q, axis, theta) + center;
}

// ----------------------------------------------------------------------------
// Deformer 4: Bend.
// pA: (axis.xyz, angle-rad-per-m)
// pB: (bendDir.xyz, _unused)
//
// We Rodrigues-rotate each vertex around c = axis × bendDir by an
// angle proportional to its projection onto bendDir — this wraps the
// slab perpendicular to `axis` around an imaginary cylinder.
// ----------------------------------------------------------------------------
static float3 opBend(float3 pos, GpuDeformOp op) {
    float3 axis    = normalize(op.pA.xyz + float3(1e-6));
    float  angle   = op.pA.w * op.header.y;
    float3 bendDir = normalize(op.pB.xyz + float3(1e-6, 0.0, 0.0));
    float3 rotAxis = normalize(cross(axis, bendDir) + float3(0.0, 1e-6, 0.0));
    float  proj    = dot(pos, bendDir);
    float  theta   = proj * angle;
    return rotateAxis(pos, rotAxis, theta);
}

// ----------------------------------------------------------------------------
// Deformer 5: Taper.
// pA: (axis.xyz, amount-per-m)
// pB: (center.xyz, clampNonNeg)
// ----------------------------------------------------------------------------
static float3 opTaper(float3 pos, GpuDeformOp op) {
    float3 axis    = normalize(op.pA.xyz + float3(1e-6));
    float  amount  = op.pA.w * op.header.y;
    float3 center  = op.pB.xyz;
    bool   doClamp = op.pB.w > 0.5;
    float3 q       = pos - center;
    float  h       = dot(q, axis);
    float  s       = 1.0 + h * amount;
    if (doClamp) s = max(s, 0.0);
    float3 axial   = axis * h;
    float3 perp    = q - axial;
    return center + axial + perp * s;
}

// ----------------------------------------------------------------------------
// Deformer 6: Wave along normal.
// pA: (dir.xyz, freq-1/m)
// pB: (speed-Hz, amount-m, displaceAlongMode, _unused)
//   displaceAlongMode: 0 = vertex normal, 1 = world up (0,0,1), 2 = dir
// ----------------------------------------------------------------------------
static float3 opWave(float3 pos, float3 vertexNormal, GpuDeformOp op) {
    float3 dir   = normalize(op.pA.xyz + float3(1e-6));
    float  freq  = op.pA.w;
    float  speed = op.pB.x;
    float  amp   = op.pB.y * op.header.y;
    int    mode  = int(op.pB.z + 0.5);
    float  t     = op.header.z;
    float  phase = dot(pos, dir) * freq - t * speed;
    float  s     = sin(phase) * amp;
    float3 along = (mode == 1) ? float3(0.0, 0.0, 1.0)
                  : (mode == 2) ? dir
                  : normalize(vertexNormal + float3(1e-6, 0.0, 0.0));
    return pos + along * s;
}

// ----------------------------------------------------------------------------
// Deformer 7: Spherify.
// pA: (center.xyz, radius)
// pB: (strength, _unused)
// ----------------------------------------------------------------------------
static float3 opSpherify(float3 pos, GpuDeformOp op) {
    float3 center   = op.pA.xyz;
    float  radius   = op.pA.w;
    float  strength = op.pB.x * op.header.y;
    float3 d        = pos - center;
    float  len      = length(d) + 1e-6;
    float3 sphere   = center + (d / len) * radius;
    return mix(pos, sphere, clamp(strength, 0.0, 1.0));
}

// ----------------------------------------------------------------------------
// Deformer 8: FFD-lite (8-point trilinear).
// pA: (bboxMin.xyz, _unused)
// pB: (bboxMax.xyz, _unused)
// corners[8]: delta from bbox corner at index i + 2j + 4k
//
// The deformed point is bbox-corner + delta, blended trilinearly by
// the vertex's normalized position in the bbox.
// ----------------------------------------------------------------------------
static float3 opFfdLite(float3 pos,
                          GpuDeformOp op,
                          constant float4* ffdCornersBase)
{
    float3 bmin = op.pA.xyz;
    float3 bmax = op.pB.xyz;
    int    slot = int(op.header.w + 0.5);
    if (slot < 0) return pos;

    float3 size = max(bmax - bmin, float3(1e-4));
    float3 u    = clamp((pos - bmin) / size, 0.0, 1.0);

    // Read 8 corner deltas for this slot. Indexed as
    // base + slot * 8 + (i + 2j + 4k).
    constant float4* slotBase = ffdCornersBase + slot * 8;
    float3 c000 = slotBase[0].xyz;
    float3 c100 = slotBase[1].xyz;
    float3 c010 = slotBase[2].xyz;
    float3 c110 = slotBase[3].xyz;
    float3 c001 = slotBase[4].xyz;
    float3 c101 = slotBase[5].xyz;
    float3 c011 = slotBase[6].xyz;
    float3 c111 = slotBase[7].xyz;

    // Original bbox corners (no displacement).
    float3 b000 = float3(bmin.x, bmin.y, bmin.z);
    float3 b100 = float3(bmax.x, bmin.y, bmin.z);
    float3 b010 = float3(bmin.x, bmax.y, bmin.z);
    float3 b110 = float3(bmax.x, bmax.y, bmin.z);
    float3 b001 = float3(bmin.x, bmin.y, bmax.z);
    float3 b101 = float3(bmax.x, bmin.y, bmax.z);
    float3 b011 = float3(bmin.x, bmax.y, bmax.z);
    float3 b111 = float3(bmax.x, bmax.y, bmax.z);

    float3 d000 = b000 + c000;
    float3 d100 = b100 + c100;
    float3 d010 = b010 + c010;
    float3 d110 = b110 + c110;
    float3 d001 = b001 + c001;
    float3 d101 = b101 + c101;
    float3 d011 = b011 + c011;
    float3 d111 = b111 + c111;

    float3 dx00 = mix(d000, d100, u.x);
    float3 dx10 = mix(d010, d110, u.x);
    float3 dx01 = mix(d001, d101, u.x);
    float3 dx11 = mix(d011, d111, u.x);
    float3 dxy0 = mix(dx00, dx10, u.y);
    float3 dxy1 = mix(dx01, dx11, u.y);
    float3 deformed = mix(dxy0, dxy1, u.z);

    return mix(pos, deformed, op.header.y);
}

// ----------------------------------------------------------------------------
// Dispatcher — apply the chain in order.
// ----------------------------------------------------------------------------
static float3 applyDeformerChain(float3 pos,
                                   float3 vertexNormal,
                                   constant GpuDeformOp* ops,
                                   int opCount,
                                   constant float4* ffdCornersBase)
{
    float3 p = pos;
    for (int i = 0; i < opCount; ++i) {
        GpuDeformOp op = ops[i];
        int type = int(op.header.x + 0.5);
        if (type == OP_NONE) continue;
        if (op.header.y <= 0.0) continue;
        if (type == OP_CURL_NOISE) {
            p = opCurlNoise(p, op);
        } else if (type == OP_VORONOI) {
            p = opVoronoi(p, op);
        } else if (type == OP_TWIST) {
            p = opTwist(p, op);
        } else if (type == OP_BEND) {
            p = opBend(p, op);
        } else if (type == OP_TAPER) {
            p = opTaper(p, op);
        } else if (type == OP_WAVE) {
            p = opWave(p, vertexNormal, op);
        } else if (type == OP_SPHERIFY) {
            p = opSpherify(p, op);
        } else if (type == OP_FFD_LITE) {
            p = opFfdLite(p, op, ffdCornersBase);
        }
    }
    return p;
}
