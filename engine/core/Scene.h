#pragma once

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

// CPU-side mesh data ready for upload to a backend (Metal/DX12).
// Positions are in mesh-local space; `transform` is mesh-local-to-world.
// glTF M2-B: positions + indices only. Normals + materials added in M2-C.
struct MeshData {
    std::string             name;
    std::vector<glm::vec3>  positions;
    std::vector<uint32_t>   indices;
    glm::mat4               transform = glm::mat4(1.0f);
};

// A loaded export from the Blender add-on. Owns CameraData + MeshData(s).
struct Scene {
    int                     outputWidth   = 1920;
    int                     outputHeight  = 1080;
    int                     schemaVersion = 0;
    std::string             sourceBlend;
    std::string             folderPath;
    CameraData              camera;
    std::vector<MeshData>   meshes;

    // Loads `scene.json` from `folderPath`, then optionally `structure.glb`
    // referenced by the manifest. Throws std::runtime_error on hard failure
    // (missing scene.json, unknown schema version, malformed JSON, glTF load
    // failure). Soft issues (missing optional fields, missing structure file)
    // log to stderr but do not throw.
    static Scene loadFromFolder(const std::string& folderPath);
};

} // namespace spacegen
