// shader_snippet.metal.inc
// =========================================================================
// AreaLightLayer — MSL fragment-shader insert for MetalRenderer.cpp's
// kStructurePbrMSL. Drop this code in *after* the spot-light loop and
// *before* the totalLight = LoDir + LoSpots + emission line.
//
// Implements representative-point area lighting per Karis 2013
// "Real Shading in Unreal Engine 4" §§ 4.1–4.3 (sphere lights + disc
// extension), plus the Lagarde / de Rousiers 2014 closed-form Lambertian
// disc form-factor for the diffuse term.
//
// Companion CPU code in AreaLightLayer.h/.cpp packs an `AreaLight`
// struct of the same layout into `u.areas[]` (see INTEGRATION.md).
// =========================================================================

// ---- Struct definition (paste in the same block as SpotLight / DirLight)
//      and grow the Uniforms struct with `AreaLight areas[MAX_AREAS]` plus
//      `.metallicCounts.y = areaCount` (.y is currently unused / 0).
//
// struct AreaLight {
//     float4 posIntensity;   // .xyz position (world),     .w intensity
//     float4 normRange;      // .xyz panel normal (unit),  .w linear range
//     float4 colorRadius;    // .rgb color,                .a equivalent radius
// };
//
// constant int MAX_AREAS = 8;          // <- declare next to MAX_SPOTS/MAX_DIRS
//
// (Inside Uniforms, add:)
// AreaLight areas[MAX_AREAS];
// =========================================================================


// ---- Area lights (representative-point, disc-equivalent) -----------------
// Inserted right after the spot-light loop in kStructurePbrMSL. Reads
// `u.metallicCounts.y` for the count (we repurpose the previously-unused
// .y channel — see INTEGRATION.md for the CPU-side change).
float3 LoAreas = float3(0.0);
int areaCount = int(u.metallicCounts.y);
int aN = min(areaCount, MAX_AREAS);
for (int i = 0; i < aN; i++) {
    AreaLight a = u.areas[i];
    float3 ap = a.posIntensity.xyz;
    float  ai = a.posIntensity.w;
    if (ai <= 0.0) continue;
    float3 an = normalize(a.normRange.xyz);   // panel normal (face direction)
    float  ar = a.normRange.w;                // linear falloff range
    float3 ac = a.colorRadius.rgb;
    float  rd = max(a.colorRadius.a, 1e-4);   // equivalent disc radius

    float3 toLight = ap - in.worldPos;
    float  dist    = length(toLight);
    if (dist > ar) continue;
    float3 Lc      = toLight / max(dist, 1e-4);   // to disc centre

    // ----- one-sided panel test -----
    // Reject points behind the panel. Note: `an` is the panel's normal;
    // we want the surface to be on the +an side. -Lc points from disc
    // centre to surface; dot(an, -Lc) > 0 means the disc faces us.
    float facing = dot(an, -Lc);
    if (facing <= 0.0) continue;

    // ----- linear range falloff (matches BeamLayer convention) -----
    float rangeFade = saturate(1.0 - dist / ar);

    // =====================================================================
    // DIFFUSE: closed-form Lambertian disc form factor.
    //   Lagarde & de Rousiers 2014 "Moving Frostbite to PBR" § 4.9.1,
    //   following Snyder 1996.  For a disc light of radius r at distance
    //   d along its centre direction, viewed from a surface with normal N:
    //
    //     formFactor ≈ max(N·Lc, 0) · r² / (d² + r²)
    //
    //   This is the Karis 2013 eq. 6 expression — exact when the disc
    //   subtends a small solid angle, very-good-approximation otherwise.
    //   Multiplied by `facing` so the diffuse term respects the one-sided
    //   panel (smooth ramp instead of hard cull, prevents popping at the
    //   silhouette).
    // =====================================================================
    float NdotLc   = max(dot(N, Lc), 0.0);
    float discR2   = rd * rd;
    float formFact = NdotLc * discR2 / max(dist * dist + discR2, 1e-4);
    float3 radDiff = ac * ai * formFact * rangeFade * facing;

    // =====================================================================
    // SPECULAR: representative-point GGX.
    //   Karis 2013 §§ 4.1 + 4.3. The reflection vector R is intersected
    //   with the disc; if it hits, we use R itself (perfect mirror); if
    //   not, we pick the closest point on the disc rim. The GGX lobe is
    //   then widened by α' = saturate(α + r / (2·dist)) to compensate
    //   for the *larger solid angle* of the area source compared to a
    //   punctual one — that's what makes the highlight soften as the
    //   panel grows or approaches.
    // =====================================================================
    float3 R          = reflect(-V, N);
    float  rDotLc     = dot(R, Lc);
    float3 onAxis     = Lc * rDotLc;          // projection of R onto Lc
    float3 offAxis    = R  - onAxis;
    float  offLen     = length(offAxis);
    // Distance from R-ray to disc rim (in the disc plane). If <= radius
    // the reflection ray pierces the disc and L_rep = R; otherwise we
    // project to the nearest point on the disc circumference.
    float3 closestOff = (offLen > 1e-5)
                      ? (offAxis / offLen) * min(offLen, rd)
                      : float3(0.0);
    float3 closestPt  = ap;                   // start at centre
    closestPt        += closestOff;           // slide on disc plane
    float3 toRep      = closestPt - in.worldPos;
    float  repDist    = length(toRep);
    float3 Lrep       = (repDist > 1e-4) ? (toRep / repDist) : Lc;

    // Roughness expansion (Karis 2013, eq. 14-ish — "sphere normalisation
    // approximation"). We use the centre distance `dist` (not repDist)
    // so the expansion is conservative on grazing angles.
    float aOrig = roughness;
    float aPrime = saturate(aOrig + rd / max(2.0 * dist, 1e-4));

    // Re-evaluate GGX with α' for the spec term; keep diffuse as above.
    // We can't call pbrEvalDirect() directly because it bundles diffuse +
    // specular under the original roughness — write the specular term
    // out explicitly.
    float NdotL = max(dot(N, Lrep), 0.0);
    float3 LoSpec = float3(0.0);
    if (NdotL > 0.0) {
        float3 H     = normalize(V + Lrep);
        float  NdotV = max(dot(N, V), 0.0);
        float  NdotH = max(dot(N, H), 0.0);
        float  VdotH = max(dot(V, H), 0.0);
        float3 F     = fresnelSchlick(VdotH, F0);
        float  D     = distributionGGX(NdotH, aPrime);
        // Energy compensation for the widened lobe: divide by the ratio
        // of the new α² to the original α². Karis' "normalize the GGX
        // peak so total energy stays constant" trick — without it the
        // highlight gets brighter as it widens (wrong).
        float  norm  = (aOrig * aOrig + 1e-5) / (aPrime * aPrime + 1e-5);
        float  G     = geometrySmith(NdotV, NdotL, aPrime);
        float3 spec  = (D * G * F * norm)
                     / max(4.0 * NdotV * NdotL, 1e-4);
        // Specular radiance uses the rep-point distance for falloff
        // consistency with the rep-point direction.
        float specFade = saturate(1.0 - repDist / ar);
        float3 radSpec = ac * ai * specFade * facing;
        LoSpec = spec * radSpec * NdotL;
    }

    // Combine. Diffuse uses the analytic form-factor (no extra NdotL —
    // it's already baked in); specular has the standard NdotL.
    float3 kdA = (1.0 - fresnelSchlick(NdotLc, F0)) * (1.0 - metallic);
    LoAreas += kdA * (baseColor / PI) * radDiff + LoSpec;
}

// ---- Splice into the existing accumulator -------------------------------
// Replace the original `totalLight = LoDir + LoSpots + emission;` line with:
//     float3 totalLight = LoDir + LoSpots + LoAreas + emission;
// =========================================================================
