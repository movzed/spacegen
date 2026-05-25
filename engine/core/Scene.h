#pragma once

#include "Layer.h"
#include "ModulatorBank.h"

#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

namespace spacegen {

// Camera matrices loaded from scene.json. All matrices are stored as glm::mat4
// (column-major). The projection matrix has been converted from Blender's
// GL-style clip-space (Z in [-1, 1]) to Metal-style (Z in [0, 1]).
struct CameraData {
    std::string name;
    glm::mat4   projection = glm::mat4(1.0f);
    glm::mat4   view       = glm::mat4(1.0f);
    glm::mat4   world      = glm::mat4(1.0f);
    float       focalLengthMM = 50.0f;
    float       fovXRad       = 0.0f;
    float       fovYRad       = 0.0f;
    float       clipStart     = 0.1f;
    float       clipEnd       = 1000.0f;
};

// Decoded image (RGBA8) ready for upload to a GPU 2D texture.
struct TextureData {
    int                   width  = 0;
    int                   height = 0;
    std::vector<uint8_t>  rgba;          // tightly packed RGBA8
    std::string           name;          // for debugging / inspector
    bool                  isSRGB = true; // baseColor + emissive are sRGB;
                                          // normal + metallicRoughness are linear
};

// PBR material data (glTF 2.0 MetallicRoughness workflow). Texture indices
// point into Scene::textures; -1 means "no texture, use factor only".
struct PbrMaterial {
    std::string  name;

    glm::vec4    baseColorFactor   = glm::vec4(1.0f);   // multiplies texture
    float        metallicFactor    = 1.0f;
    float        roughnessFactor   = 1.0f;
    glm::vec3    emissiveFactor    = glm::vec3(0.0f);
    float        normalScale       = 1.0f;
    float        occlusionStrength = 1.0f;

    int          baseColorTex         = -1;  // sRGB
    int          metallicRoughnessTex = -1;  // linear: G=roughness, B=metallic
    int          normalTex            = -1;  // linear, tangent-space
    int          emissiveTex          = -1;  // sRGB
    int          occlusionTex         = -1;  // linear, R-channel
};

// CPU-side mesh data ready for upload to a backend (Metal/DX12).
// Positions + normals are in mesh-local space; `transform` is mesh-local-to-world.
struct MeshData {
    std::string             name;
    std::vector<glm::vec3>  positions;
    std::vector<glm::vec3>  normals;     // empty if glTF primitive had no NORMAL attribute
    std::vector<glm::vec2>  uvs;         // empty if no TEXCOORD_0 (materials)
    std::vector<glm::vec2>  uvs1;        // empty if no TEXCOORD_1 (Syphon / overlay)
    std::vector<glm::vec3>  tangents;    // empty if no TANGENT attribute (for normal mapping)
    std::vector<uint32_t>   indices;
    glm::mat4               transform   = glm::mat4(1.0f);
    int                     materialIdx = -1; // index into Scene::materials, -1 = default
};

// A realtime light, owned by SpaceGen (NOT exported from Blender — Blender
// lights are ignored per the data-contract-blender-engine skill).
struct Light {
    enum class Type { Directional, Point, Spot };
    Type        type      = Type::Directional;
    std::string name;
    // World-space direction (FROM the light TOWARDS the scene). Used by
    // Directional and Spot. Should be normalized.
    glm::vec3   direction = glm::vec3(0.5f, -0.7f, -0.5f);
    // World-space position. Used by Point and Spot.
    glm::vec3   position  = glm::vec3(0.0f);
    // Radiance multiplier (in arbitrary linear units; tune in UI).
    glm::vec3   color     = glm::vec3(1.0f);
    float       intensity = 1.0f;
    // Spot cone (cosine of inner/outer half-angles).
    float       coneInnerCos = 0.97f;
    float       coneOuterCos = 0.95f;
    // Point/Spot range for inverse-square attenuation softening.
    float       range        = 100.0f;
};

// A loaded export from the Blender add-on. Owns CameraData + MeshData(s) +
// optional Lights (added at runtime, NOT exported from Blender) + a Bus
// of layers (StructureLayer + N BeamLayers + future FX layers).
struct Scene {
    int                     outputWidth   = 1920;
    int                     outputHeight  = 1080;
    int                     schemaVersion = 0;
    std::string             sourceBlend;
    std::string             folderPath;
    CameraData                  camera;
    std::vector<MeshData>       meshes;
    std::vector<PbrMaterial>    materials;
    std::vector<TextureData>    textures;
    std::vector<Light>          lights;
    Bus                         bus;
    ModulatorBank               modulators;

    // Loads `scene.json` from `folderPath`, then optionally `structure.glb`
    // referenced by the manifest. Throws std::runtime_error on hard failure
    // (missing scene.json, unknown schema version, malformed JSON, glTF load
    // failure). Soft issues (missing optional fields, missing structure file)
    // log to stderr but do not throw.
    static Scene loadFromFolder(const std::string& folderPath);
};

} // namespace spacegen
