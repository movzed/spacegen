#include "Scene.h"

#include <nlohmann/json.hpp>
#include <tiny_gltf.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace spacegen {

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

constexpr int kKnownSchemaVersion = 1;

// JSON row-major mat4 (Blender export) -> glm::mat4 (column-major).
glm::mat4 jsonRowMajorToMat4(const json& j) {
    glm::mat4 m(1.0f);
    if (!j.is_array() || j.size() != 4) {
        throw std::runtime_error("matrix JSON must be a 4x4 array of arrays");
    }
    for (int row = 0; row < 4; ++row) {
        const auto& r = j[row];
        if (!r.is_array() || r.size() != 4) {
            throw std::runtime_error("matrix row must be array of 4");
        }
        for (int col = 0; col < 4; ++col) {
            m[col][row] = r[col].get<float>();
        }
    }
    return m;
}

// Convert OpenGL-style projection (clip Z in [-1, 1]) to Metal/D3D-style
// (clip Z in [0, 1]). Blender's calc_matrix_camera() yields GL-style; Metal
// and DX12 expect [0, 1]. The fix matrix remaps the third row.
glm::mat4 glProjectionToMetalProjection(const glm::mat4& gl) {
    glm::mat4 zFix(1.0f);
    zFix[2][2] = 0.5f;
    zFix[3][2] = 0.5f;
    return zFix * gl;
}

// glTF node -> local matrix. Either a direct matrix or TRS components.
glm::mat4 nodeLocalMatrix(const tinygltf::Node& node) {
    if (node.matrix.size() == 16) {
        // glTF stores matrices column-major; tinygltf returns 16 doubles in
        // column-major order (m[col*4 + row]).
        glm::mat4 m;
        for (int col = 0; col < 4; ++col) {
            for (int row = 0; row < 4; ++row) {
                m[col][row] = static_cast<float>(node.matrix[col * 4 + row]);
            }
        }
        return m;
    }
    glm::mat4 m(1.0f);
    if (node.translation.size() == 3) {
        m = glm::translate(m, glm::vec3(
            static_cast<float>(node.translation[0]),
            static_cast<float>(node.translation[1]),
            static_cast<float>(node.translation[2])));
    }
    if (node.rotation.size() == 4) {
        // glTF rotation order: x, y, z, w. GLM quat constructor: w, x, y, z.
        glm::quat q(
            static_cast<float>(node.rotation[3]),  // w
            static_cast<float>(node.rotation[0]),
            static_cast<float>(node.rotation[1]),
            static_cast<float>(node.rotation[2]));
        m = m * glm::mat4_cast(q);
    }
    if (node.scale.size() == 3) {
        m = glm::scale(m, glm::vec3(
            static_cast<float>(node.scale[0]),
            static_cast<float>(node.scale[1]),
            static_cast<float>(node.scale[2])));
    }
    return m;
}

void processPrimitive(const tinygltf::Model& model,
                      const tinygltf::Primitive& prim,
                      const glm::mat4& worldMatrix,
                      const std::string& meshName,
                      std::vector<MeshData>& outMeshes)
{
    auto posIt = prim.attributes.find("POSITION");
    if (posIt == prim.attributes.end()) {
        std::cerr << "[Scene] Primitive of mesh '" << meshName
                  << "' has no POSITION. Skipping.\n";
        return;
    }
    const auto& posAccessor = model.accessors[posIt->second];
    const auto& posView     = model.bufferViews[posAccessor.bufferView];
    const auto& posBuffer   = model.buffers[posView.buffer];
    const auto* posBytes = posBuffer.data.data()
        + posView.byteOffset + posAccessor.byteOffset;
    const size_t stride = posView.byteStride > 0
        ? posView.byteStride
        : sizeof(float) * 3;

    MeshData out;
    out.name = meshName.empty() ? std::string("mesh") : meshName;
    out.transform = worldMatrix;
    std::printf("[Scene]   Processing '%s': %zu verts...\n",
                 out.name.c_str(), posAccessor.count);
    std::fflush(stdout);
    out.positions.resize(posAccessor.count);
    if (stride == sizeof(float) * 3) {
        // Tightly packed: single memcpy
        std::memcpy(out.positions.data(), posBytes,
                    sizeof(float) * 3 * posAccessor.count);
    } else {
        for (size_t i = 0; i < posAccessor.count; ++i) {
            const float* p = reinterpret_cast<const float*>(posBytes + i * stride);
            out.positions[i] = glm::vec3(p[0], p[1], p[2]);
        }
    }

    if (prim.indices >= 0) {
        const auto& idxAccessor = model.accessors[prim.indices];
        const auto& idxView     = model.bufferViews[idxAccessor.bufferView];
        const auto& idxBuffer   = model.buffers[idxView.buffer];
        const uint8_t* idxBytes = idxBuffer.data.data()
            + idxView.byteOffset + idxAccessor.byteOffset;
        std::printf("[Scene]   Processing '%s': %zu indices (componentType=%d)...\n",
                     out.name.c_str(), idxAccessor.count, idxAccessor.componentType);
        std::fflush(stdout);
        out.indices.resize(idxAccessor.count);
        switch (idxAccessor.componentType) {
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
                for (size_t i = 0; i < idxAccessor.count; ++i) {
                    out.indices[i] = idxBytes[i];
                }
                break;
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: {
                const uint16_t* p = reinterpret_cast<const uint16_t*>(idxBytes);
                for (size_t i = 0; i < idxAccessor.count; ++i) {
                    out.indices[i] = p[i];
                }
                break;
            }
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT: {
                const uint32_t* p = reinterpret_cast<const uint32_t*>(idxBytes);
                std::memcpy(out.indices.data(), p,
                            sizeof(uint32_t) * idxAccessor.count);
                break;
            }
            default:
                std::cerr << "[Scene] Unsupported index componentType "
                          << idxAccessor.componentType
                          << " in '" << meshName << "'. Skipping primitive.\n";
                return;
        }
    } else {
        // Non-indexed
        out.indices.reserve(posAccessor.count);
        for (size_t i = 0; i < posAccessor.count; ++i) {
            out.indices.push_back(static_cast<uint32_t>(i));
        }
    }

    std::printf("[Scene]   Done with primitive '%s'. Storing...\n",
                 out.name.c_str());
    std::fflush(stdout);
    outMeshes.push_back(std::move(out));
    std::printf("[Scene]   Stored.\n");
    std::fflush(stdout);
}

void walkNode(const tinygltf::Model& model,
              int nodeIdx,
              const glm::mat4& parentMatrix,
              std::vector<MeshData>& outMeshes)
{
    if (nodeIdx < 0 || nodeIdx >= static_cast<int>(model.nodes.size())) {
        return;
    }
    const auto& node = model.nodes[nodeIdx];
    glm::mat4 worldMatrix = parentMatrix * nodeLocalMatrix(node);

    if (node.mesh >= 0 && node.mesh < static_cast<int>(model.meshes.size())) {
        const auto& mesh = model.meshes[node.mesh];
        for (const auto& prim : mesh.primitives) {
            processPrimitive(model, prim, worldMatrix, mesh.name, outMeshes);
        }
    }

    for (int child : node.children) {
        walkNode(model, child, worldMatrix, outMeshes);
    }
}

} // namespace

Scene Scene::loadFromFolder(const std::string& folderPath) {
    fs::path dir(folderPath);
    fs::path scenePath = dir / "scene.json";

    if (!fs::exists(scenePath)) {
        throw std::runtime_error("scene.json not found in " + folderPath);
    }

    std::ifstream f(scenePath);
    if (!f) {
        throw std::runtime_error("cannot open " + scenePath.string());
    }
    json j;
    try {
        f >> j;
    } catch (const std::exception& e) {
        throw std::runtime_error("scene.json parse failed: " + std::string(e.what()));
    }

    Scene scene;
    scene.folderPath = folderPath;
    scene.schemaVersion = j.value("schema_version", 0);
    if (scene.schemaVersion > kKnownSchemaVersion) {
        throw std::runtime_error(
            "scene.json schema_version=" + std::to_string(scene.schemaVersion)
            + " exceeds known major=" + std::to_string(kKnownSchemaVersion)
            + ". Update the engine, or re-export with an older add-on.");
    }
    if (scene.schemaVersion < kKnownSchemaVersion) {
        std::cerr << "[Scene] scene.json schema_version="
                  << scene.schemaVersion
                  << " is older than known=" << kKnownSchemaVersion
                  << ". Defaults will be used for missing fields.\n";
    }

    scene.sourceBlend = j.value("source_blend", "");
    if (j.contains("output_resolution") && j["output_resolution"].is_array()
        && j["output_resolution"].size() == 2) {
        scene.outputWidth  = j["output_resolution"][0].get<int>();
        scene.outputHeight = j["output_resolution"][1].get<int>();
    }

    // ---- Camera ----
    if (!j.contains("camera")) {
        throw std::runtime_error("scene.json missing required field 'camera'");
    }
    const auto& cj = j.at("camera");
    std::string camType = cj.value("type", "PERSP");
    if (camType != "PERSP") {
        // The projection_matrix is authoritative — we consume it verbatim
        // regardless of camera type. ORTHO produces a valid orthographic
        // matrix; PANO/PERSP equirectangular is not supported and would
        // produce a bad matrix, but we still attempt to render rather than
        // hard-fail. Operator should fix the Blender side if visuals are off.
        std::cerr << "[Scene] camera.type='" << camType
                  << "' (expected PERSP). Continuing with the exported "
                     "projection matrix as-is.\n";
    }
    scene.camera.name          = cj.value("name", std::string("Camera"));
    scene.camera.focalLengthMM = cj.value("focal_length_mm", 50.0f);
    scene.camera.fovXRad       = cj.value("fov_x_rad", 0.0f);
    scene.camera.fovYRad       = cj.value("fov_y_rad", 0.0f);
    scene.camera.clipStart     = cj.value("clip_start", 0.1f);
    scene.camera.clipEnd       = cj.value("clip_end", 1000.0f);

    glm::mat4 worldM = jsonRowMajorToMat4(cj.at("world_matrix"));
    glm::mat4 viewM  = jsonRowMajorToMat4(cj.at("view_matrix"));
    glm::mat4 projGL = jsonRowMajorToMat4(cj.at("projection_matrix"));

    scene.camera.world      = worldM;
    scene.camera.view       = viewM;
    scene.camera.projection = glProjectionToMetalProjection(projGL);

    // ---- Structure mesh ----
    if (j.contains("structure")
        && j["structure"].contains("file")
        && !j["structure"]["file"].is_null())
    {
        std::string glbFile = j["structure"]["file"].get<std::string>();
        fs::path glbPath = dir / glbFile;

        if (!fs::exists(glbPath)) {
            std::cerr << "[Scene] structure.file referenced but not found: "
                      << glbPath.string() << "\n";
        } else {
            std::printf("[Scene] Loading glTF binary: %s (%.1f MB on disk)...\n",
                         glbPath.string().c_str(),
                         fs::file_size(glbPath) / (1024.0 * 1024.0));
            std::fflush(stdout);
            tinygltf::TinyGLTF loader;
            tinygltf::Model model;
            std::string err, warn;
            bool ok = loader.LoadBinaryFromFile(&model, &err, &warn,
                                                 glbPath.string());
            std::printf("[Scene] glTF parsed: %zu meshes, %zu nodes, %zu buffers.\n",
                         model.meshes.size(), model.nodes.size(),
                         model.buffers.size());
            std::fflush(stdout);
            if (!warn.empty()) {
                std::cerr << "[Scene] glTF warn: " << warn << "\n";
            }
            if (!ok) {
                throw std::runtime_error(
                    "Failed to load " + glbPath.string() + ": " + err);
            }

            int sceneIdx = model.defaultScene;
            if (sceneIdx < 0) sceneIdx = 0;
            if (sceneIdx < static_cast<int>(model.scenes.size())) {
                for (int rootNode : model.scenes[sceneIdx].nodes) {
                    walkNode(model, rootNode, glm::mat4(1.0f), scene.meshes);
                }
            } else {
                std::cerr << "[Scene] glTF has no scene; walking all top-level"
                             " nodes.\n";
                for (size_t i = 0; i < model.nodes.size(); ++i) {
                    walkNode(model, static_cast<int>(i), glm::mat4(1.0f),
                             scene.meshes);
                }
            }
            std::printf("[Scene] Node walk complete. About to free glTF Model...\n");
            std::fflush(stdout);
        }
    }
    std::printf("[Scene] loadFromFolder returning. %zu meshes in Scene.\n",
                 scene.meshes.size());
    std::fflush(stdout);
    return scene;
}

} // namespace spacegen
