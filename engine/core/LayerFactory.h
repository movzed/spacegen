#pragma once
// LayerFactory — maps an ILayer::typeName() string back to a freshly
// constructed concrete layer. This is the inverse of the "+ Foo" buttons in
// the Layer Rack: it lets ScenePreset rebuild a saved layer stack purely from
// the serialized typeName, without the UI knowing every concrete type.
//
// HEADER-ONLY (inline). No CMakeLists change needed — including this file
// pulls in every concrete layer header so the existing TU (Workstation.mm)
// already links all the types.
//
// CONTRACT: the strings here MUST stay in sync with each layer's
// typeName() override. If a new layer type is added, add one line below.
// An unknown typeName returns nullptr (caller skips it gracefully).

#include <memory>
#include <string>

#include "Layer.h"

// Generators / scene layers (core/).
#include "StructureLayer.h"
#include "BeamLayer.h"
#include "DirectionalLightLayer.h"
#include "AmbientLightLayer.h"
#include "SyphonInputLayer.h"

// Effect layers (effects/).
#include "../effects/area_light/AreaLightLayer.h"
#include "../effects/light_cloner/LightClonerLayer.h"
#include "../effects/procedural_material/ProceduralMaterialLayer.h"
#include "../effects/mesh_deformation/MeshDeformationLayer.h"
#include "../effects/mesh_fracture/MeshFractureLayer.h"
#include "../effects/hologram/HologramMaterialLayer.h"
#include "../effects/bloom/BloomEffect.h"
#include "../effects/motion_blur/MotionBlurEffect.h"
#include "../effects/post_fx/ChromaticAberrationEffect.h"
#include "../effects/post_fx/GlitchEffect.h"
#include "../effects/sdf_particles/SDFParticleLayer.h"
#include "../effects/volumetric_beams/VolumetricBeamLayer.h"

namespace spacegen {

// Construct a default-initialized layer for the given typeName() string.
// Returns nullptr for an unknown type (forward/backward-compat: a preset
// saved by a newer build that references a type this build lacks is skipped,
// not fatal).
inline std::unique_ptr<ILayer> makeLayerByTypeName(const std::string& t) {
    // ---- Generators / scene layers ----
    if (t == "Structure")          return std::make_unique<StructureLayer>();
    if (t == "Spot Light")         return std::make_unique<BeamLayer>();
    if (t == "Directional Light")  return std::make_unique<DirectionalLightLayer>();
    if (t == "Ambient Fill")       return std::make_unique<AmbientLightLayer>();
    if (t == "Syphon Input")       return std::make_unique<SyphonInputLayer>();

    // ---- Effect / surface layers ----
    if (t == "Area Light")         return std::make_unique<AreaLightLayer>();
    if (t == "Light Cloner")       return std::make_unique<LightClonerLayer>();
    if (t == "ProceduralMaterial") return std::make_unique<ProceduralMaterialLayer>();
    if (t == "Mesh Deformation")   return std::make_unique<MeshDeformationLayer>();
    if (t == "Mesh Fracture")      return std::make_unique<MeshFractureLayer>();
    if (t == "Hologram Material")  return std::make_unique<HologramMaterialLayer>();
    if (t == "SDF Particles")      return std::make_unique<SDFParticleLayer>();

    // ---- Post-FX (screen-space) ----
    if (t == "Bloom (Karis/Jimenez)") return std::make_unique<BloomEffect>();
    if (t == "Motion Blur")           return std::make_unique<MotionBlurEffect>();
    if (t == "Chromatic Aberration")  return std::make_unique<ChromaticAberrationEffect>();
    if (t == "Glitch")                return std::make_unique<GlitchEffect>();
    if (t == "Volumetric Beams")      return std::make_unique<VolumetricBeamLayer>();

    return nullptr;  // unknown type — caller skips
}

} // namespace spacegen
