# AreaLightLayer

A finite-extent area light (rectangle or disc) that **orbits** the loaded
structure mesh along one of several parametric paths. Contributes a
representative-point PBR term (Karis 2013 § 4.2) to the existing forward
structure shader, integrated alongside spot lights and directional lights.

The defining hard constraint:

> **The light NEVER passes through the structure mesh; it only orbits
> around it. The operator must never see the "disappearance" effect of a
> light crossing geometry.**

This is enforced at the layer's `update()` step by projecting the
parametric position onto an *orbit sphere* of radius
`max(scene.bbox.maxDim) × 1.2` centred on `scene.centroid`. The sphere
fully encloses the mesh's world-space AABB, so the area-light reference
point is provably outside the convex hull of the geometry at every
frame.

---

## 1. Why "orbit only", not "free-fly"?

In a projection-mapping context the structure mesh **is** the physical
object the operator points a video projector at. The light source is a
virtual abstraction whose only job is to modulate the surface BRDF.

If a freely-positioned light were allowed to cross the mesh, two
visually catastrophic things happen on the projected output:

1. The light's *world position* moves to the back side of the mesh.
   `N · L` flips sign on the previously-lit faces, the surface goes
   pitch black, then re-illuminates on the back. Operator perceives a
   flash + dark frame.
2. The faces opposite the new position become lit — but those faces are
   not visible to the operator (camera/projector is on the front side),
   so the operator's monitor goes dark while the back lights up
   invisibly.

A *bracketed* orbit (light radius locked > scene bound radius)
sidesteps both problems: the light always shines onto the side the
projector sees, and animation is naturally periodic.

---

## 2. Orbit modes

All modes evaluate a parametric `path(t) → vec3` in **local sphere
space** (origin at scene centroid, unit sphere). The result is
normalised onto the unit sphere, scaled by `orbitRadius` (clamped to be
≥ `safeRadius`), and translated back into world space.

`safeRadius = max(bbox.maxDim) × safetyFactor` where `safetyFactor`
defaults to 1.2 (20 % stand-off).

### 2.1 Circular orbit
Closed-form rotation around one of the world axes (X / Y / Z) with
angular velocity `speedDegPerSec`.

```
phi   = speedDegPerSec * t * π/180
p     = R · (cos φ, sin φ, 0)        if axis == Z
        R · (cos φ, 0, sin φ)        if axis == Y
        R · (0, cos φ, sin φ)        if axis == X
```

### 2.2 Lissajous figure
Two-axis sinusoidal motion on a 2D slice of the sphere, lifted to 3D by
the chosen up axis.

```
x(t) = Ax · sin(a · t)
y(t) = Ay · sin(b · t + φ)
```

with `a, b ∈ ℝ⁺` (typically small integer ratios for closed curves) and
`φ ∈ [0, 2π)`. The (x, y) pair becomes a tangent-plane vector at the
"north pole" of the chosen axis; we *project it onto the sphere* via
stereographic-like wrap rather than a planar offset so the trajectory
follows the surface cleanly without ever crossing the centroid.

### 2.3 Helix (bob + horizontal orbit)
Combines a circular azimuthal orbit at speed `speedDegPerSec` with a
vertical bob along the orbit axis at `bobFreqHz` and `bobAmp` (in units
of `safeRadius`). The XY radius shrinks so the world distance to centre
stays exactly `orbitRadius`:

```
z(t) = bobAmp · sin(2π · bobFreqHz · t)
xyR  = √(1 − z²)
x(t) = xyR · cos φ
y(t) = xyR · sin φ
```

After normalisation by `orbitRadius`, the locus is identical to a
constant-radius spherical helix.

### 2.4 Random points on sphere (smooth lerp)
At intervals of `dwellSeconds`, samples a uniform random point on the
unit sphere using Marsaglia's method (rejection on the unit disc then
lift). Between samples, smoothly **slerps** between the previous and
next sample with a smoothstep-eased parameter. This produces the
classic "random-look chase" effect (think Tomorrowland intro lights)
while staying perfectly on the sphere.

---

## 3. Position clamp (the hard guarantee)

After the orbit mode produces a *raw* position `p_raw` in world space,
we apply the safety clamp:

```cpp
glm::vec3 toLight = p_raw - centroid;
float     dist    = glm::length(toLight);
float     safe    = std::max(safeRadius, orbitRadius);
glm::vec3 dir     = (dist > 1e-5f) ? (toLight / dist)
                                   : glm::vec3(0, 0, 1);  // fallback
position          = centroid + dir * safe;
```

`safeRadius` is computed once per scene load from the AABB:

```cpp
glm::vec3 extent = scene.bboxMax - scene.bboxMin;
float maxDim     = std::max({extent.x, extent.y, extent.z});
float safeRadius = 0.5f * maxDim * safetyFactor;     // half-extent × factor
```

For a centred AABB, the inscribed sphere of the AABB has radius
`max(extent) / 2`. The **circumscribed** sphere of the same AABB has
radius `|extent| / 2 = 0.5 · √(x² + y² + z²)`. We use the
circumscribed-sphere radius (or `maxDim × safetyFactor / 2`, whichever
is larger), so any rotation of the AABB still fits inside the orbit
sphere. With `safetyFactor = 1.2` the light stand-off is at least 20 %
larger than the diagonal radius, generous enough to never visually
clip a complex mesh either.

This guarantee is **unconditional**: even if the operator drags
`orbitRadius` to zero, the clamp kicks in and replaces it with
`safeRadius`. The light cannot enter the mesh AABB; therefore it
cannot enter the mesh.

---

## 4. PBR contribution — analytic disc / representative-point

We follow Karis 2013, *Real Shading in Unreal Engine 4* (SIGGRAPH 2013
PBS course), § 4.2: "Sphere Lights" and the disc-light extension in
§ 4.3. The trick is to choose a **representative point** on the area
shape — the point that "best represents" the integral of the
distribution-aligned BRDF over the source area — and evaluate the
existing punctual-light formula at that point with a Smith-G
normalisation correction for the increased solid angle.

### 4.1 Diffuse (Lambert) — closed-form for disc

For a Lambertian surface and a disc light of radius `r` at distance
`d` along the disc normal `n_disc`, with surface normal `N` and to-
centre direction `L`:

```
form_factor ≈ (N · L) · r² / (d² + r²)              (Karis eq. 6)
diffuse     ≈ (baseColor / π) · radiance · form_factor
```

We use this directly when the shape is a disc; for a rectangle we
approximate with the equivalent-area disc
`r_eq = √((width · height) / π)` — Karis explicitly notes this is a
"good enough" hack for moving stage-light contexts. For the precise
five-sample analytic integration of a rectangle, see Heitz et al. 2016
*Real-Time Polygonal-Light Shading with Linearly Transformed Cosines*
(LTC) — out of scope here; we can add an LTC LUT layer in a later
milestone.

### 4.2 Specular — representative-point

For the GGX specular lobe we replace the punctual `L` direction with
`L_rep`, the point on the light disc that most closely aligns with the
**reflection** of the view vector through the surface normal:

```
R          = reflect(-V, N)
centre_dir = L                     (to disc centre)
to_R       = R - centre_dir·dot(centre_dir, R)        (R projected onto disc plane)
to_R_len   = length(to_R)
closest    = (to_R_len <= radius) ? R                    (R hits the disc — perfect mirror)
                                  : centre_dir + (to_R / to_R_len) * radius
L_rep      = normalize(closest)
```

The energy must be **renormalised** to account for the larger solid
angle of the area source vs. a punctual one. Karis introduces a
roughness term `α' = saturate(α + r/(2·dist))` so the GGX peak widens
with light radius; this is what eliminates the unphysical pinhole
specular spike and visually "softens" the highlight as the source
grows.

The MSL implementation is in `shader_snippet.metal.inc`.

### 4.3 Falloff
Linear within `range` (matching `BeamLayer`'s projection-mapping
falloff convention — no inverse-square; operator wants predictable
intensity at typical 5-20 m distances):

```
attenuation = saturate(1 − dist / range)
```

### 4.4 Back-face culling
We early-out when `dot(N, L) ≤ 0` (Lambert) **and** when the surface
faces away from the disc normal: `dot(N_disc, -L) ≤ 0`. The latter
ensures only points "in front of" the disc receive light, matching the
real-world behaviour of a one-sided panel.

---

## 5. ImGui inspector

All parameters editable live:

```
SHAPE
  ◯ Disc   ◯ Rectangle
  radius / width × height
  pan, tilt   (disc normal orientation; default = "face centroid")
  faceCentroid (bool, default true — disc normal auto-points at centroid)
LIGHT
  color (HSV wheel)
  intensity     [0..30]
  range (m)     [0.5..200]
ORBIT MODE
  Combo: Circular / Lissajous / Helix / Random
  Axis (X/Y/Z) — Circular + Helix
  Speed (deg/s) — Circular + Helix
  Ax, Ay, a, b, phi — Lissajous
  bobFreq, bobAmp — Helix
  dwellSeconds, slerpEase — Random
ORBIT SAFETY
  Safety factor [1.05..3.0]
  Orbit radius (m)  (clamped >= safeRadius shown live)
DEBUG
  ◯ Draw orbit gizmo (TODO future)
```

---

## 6. References

- **Karis, Brian.** *Real Shading in Unreal Engine 4.* SIGGRAPH 2013,
  Physically Based Shading in Theory and Practice course. Sections
  4.1–4.3 cover sphere lights, disc/tube lights, and the
  representative-point approximation. The α' = saturate(α + r/(2·d))
  roughness expansion trick is from § 4.1, "Sphere Lights".
  https://blog.selfshadow.com/publications/s2013-shading-course/karis/s2013_pbs_epic_notes_v2.pdf
- **Heitz, Hill, McGuire 2016.** *Real-Time Polygonal-Light Shading
  with Linearly Transformed Cosines.* SIGGRAPH 2016. (For future LTC
  upgrade of the rectangle path.)
- **Marsaglia, G. 1972.** *Choosing a Point from the Surface of a
  Sphere.* Ann. Math. Statist. 43(2). (Uniform sphere sampling.)
- **Lagarde & de Rousiers 2014.** *Moving Frostbite to PBR.* SIGGRAPH
  2014. § 4.9.1 has the closed-form Lambertian disc integral we use
  for the diffuse term.
- **Inigo Quilez** — *signed distance to a disc.*
  https://iquilezles.org/articles/distfunctions/ (used only for the
  intuition of "closest point on disc"; the actual code is the cheap
  Karis projection.)
- SpaceGen-internal: `BeamLayer.h/cpp`, `DirectionalLightLayer.h/cpp`,
  `MetalRenderer.cpp::kStructurePbrMSL` (the spot-light loop is the
  template for our area-light loop).

---

## 7. Files

```
area_light/
├── README.md               (this file)
├── AreaLightLayer.h        ILayer subclass declaration
├── AreaLightLayer.cpp      Implementation (orbit math + ImGui)
├── shader_snippet.metal.inc MSL fragment shader insert
└── INTEGRATION.md          Exact code edits to wire it up
```

Total: ~550 LOC across the four code files (C++ + MSL).
