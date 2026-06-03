#include "Scene.h"

#include "UvAtlas.h"

#include <nlohmann/json.hpp>
#include <tiny_gltf.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
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
    out.positions.resize(posAccessor.count);
    if (stride == sizeof(float) * 3) {
        std::memcpy(out.positions.data(), posBytes,
                    sizeof(float) * 3 * posAccessor.count);
    } else {
        for (size_t i = 0; i < posAccessor.count; ++i) {
            const float* p = reinterpret_cast<const float*>(posBytes + i * stride);
            out.positions[i] = glm::vec3(p[0], p[1], p[2]);
        }
    }

    // NORMAL attribute (optional but expected — we requested export_normals=True)
    auto nrmIt = prim.attributes.find("NORMAL");
    if (nrmIt != prim.attributes.end()) {
        const auto& nrmAccessor = model.accessors[nrmIt->second];
        const auto& nrmView     = model.bufferViews[nrmAccessor.bufferView];
        const auto& nrmBuffer   = model.buffers[nrmView.buffer];
        const auto* nrmBytes = nrmBuffer.data.data()
            + nrmView.byteOffset + nrmAccessor.byteOffset;
        const size_t nrmStride = nrmView.byteStride > 0
            ? nrmView.byteStride
            : sizeof(float) * 3;
        out.normals.resize(nrmAccessor.count);
        if (nrmStride == sizeof(float) * 3
            && nrmAccessor.count == posAccessor.count) {
            std::memcpy(out.normals.data(), nrmBytes,
                        sizeof(float) * 3 * nrmAccessor.count);
        } else {
            for (size_t i = 0; i < nrmAccessor.count; ++i) {
                const float* p = reinterpret_cast<const float*>(nrmBytes + i * nrmStride);
                out.normals[i] = glm::vec3(p[0], p[1], p[2]);
            }
        }
    } else {
        std::cerr << "[Scene] Mesh '" << out.name
                  << "' has no NORMAL attribute. Lighting will be undefined.\n";
    }

    // TEXCOORD_0 attribute (UVs for texture sampling). Optional — falls
    // back to (0,0) per vertex when absent.
    auto uvIt = prim.attributes.find("TEXCOORD_0");
    if (uvIt != prim.attributes.end()) {
        const auto& uvAcc = model.accessors[uvIt->second];
        const auto& uvView = model.bufferViews[uvAcc.bufferView];
        const auto& uvBuf  = model.buffers[uvView.buffer];
        const auto* uvBytes = uvBuf.data.data()
            + uvView.byteOffset + uvAcc.byteOffset;
        const size_t uvStride = uvView.byteStride > 0
            ? uvView.byteStride
            : sizeof(float) * 2;
        out.uvs.resize(uvAcc.count);
        if (uvStride == sizeof(float) * 2
            && uvAcc.count == posAccessor.count) {
            std::memcpy(out.uvs.data(), uvBytes,
                        sizeof(float) * 2 * uvAcc.count);
        } else {
            for (size_t i = 0; i < uvAcc.count; ++i) {
                const float* p = reinterpret_cast<const float*>(uvBytes + i * uvStride);
                out.uvs[i] = glm::vec2(p[0], p[1]);
            }
        }
    }

    // TEXCOORD_1 attribute (secondary UV set, used by SyphonInputLayer to
    // project live video onto faces whose primary UVs are degenerate /
    // missing). Same extraction shape as TEXCOORD_0. Optional — absence
    // means the engine will fall back to UV0 or another mapping mode.
    auto uv1It = prim.attributes.find("TEXCOORD_1");
    if (uv1It != prim.attributes.end()) {
        const auto& uvAcc = model.accessors[uv1It->second];
        const auto& uvView = model.bufferViews[uvAcc.bufferView];
        const auto& uvBuf  = model.buffers[uvView.buffer];
        const auto* uvBytes = uvBuf.data.data()
            + uvView.byteOffset + uvAcc.byteOffset;
        const size_t uvStride = uvView.byteStride > 0
            ? uvView.byteStride
            : sizeof(float) * 2;
        out.uvs1.resize(uvAcc.count);
        if (uvStride == sizeof(float) * 2
            && uvAcc.count == posAccessor.count) {
            std::memcpy(out.uvs1.data(), uvBytes,
                        sizeof(float) * 2 * uvAcc.count);
        } else {
            for (size_t i = 0; i < uvAcc.count; ++i) {
                const float* p = reinterpret_cast<const float*>(uvBytes + i * uvStride);
                out.uvs1[i] = glm::vec2(p[0], p[1]);
            }
        }
        std::cerr << "[Scene] Mesh '" << out.name
                  << "' has TEXCOORD_1 (" << uvAcc.count
                  << " uvs) — using as SyphonUV.\n";
    }

    // TANGENT attribute (for normal mapping). Optional.
    auto tanIt = prim.attributes.find("TANGENT");
    if (tanIt != prim.attributes.end()) {
        const auto& tAcc = model.accessors[tanIt->second];
        const auto& tView = model.bufferViews[tAcc.bufferView];
        const auto& tBuf  = model.buffers[tView.buffer];
        const auto* tBytes = tBuf.data.data()
            + tView.byteOffset + tAcc.byteOffset;
        // glTF tangents are vec4 (xyz + handedness w). We only keep xyz
        // for now and assume right-handed; full bitangent computation
        // (cross(normal, tangent) * w) lives in the shader if needed.
        const size_t tStride = tView.byteStride > 0
            ? tView.byteStride
            : sizeof(float) * 4;
        out.tangents.resize(tAcc.count);
        for (size_t i = 0; i < tAcc.count; ++i) {
            const float* p = reinterpret_cast<const float*>(tBytes + i * tStride);
            out.tangents[i] = glm::vec3(p[0], p[1], p[2]);
        }
    }

    // Material index (-1 = use default material)
    out.materialIdx = prim.material;

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

// Decode tinygltf images into RGBA8 TextureData. tinygltf already decoded
// the bytes (we built it with STB_IMAGE_IMPLEMENTATION), so each image has
// `image` = raw decoded pixels. We re-pack to RGBA8 for uniform handling.
void extractTextures(const tinygltf::Model& model,
                      std::vector<TextureData>& outTextures,
                      const std::vector<bool>& sRGBFlags)
{
    outTextures.clear();
    outTextures.reserve(model.images.size());
    for (size_t i = 0; i < model.images.size(); ++i) {
        const auto& img = model.images[i];
        TextureData td;
        td.name   = img.name.empty()
            ? ("image_" + std::to_string(i))
            : img.name;
        td.width  = img.width;
        td.height = img.height;
        td.isSRGB = (i < sRGBFlags.size()) ? sRGBFlags[i] : true;

        if (img.width <= 0 || img.height <= 0 || img.image.empty()) {
            std::cerr << "[Scene] Image '" << td.name
                      << "' is empty/undecoded. Skipping.\n";
            outTextures.push_back(std::move(td));  // keep slot for indexing
            continue;
        }

        const int n = img.component;            // 1, 3 or 4
        const int pxCount = img.width * img.height;
        td.rgba.resize(static_cast<size_t>(pxCount) * 4);
        for (int p = 0; p < pxCount; ++p) {
            const uint8_t* src = &img.image[static_cast<size_t>(p) * n];
            uint8_t* dst = &td.rgba[static_cast<size_t>(p) * 4];
            if (n == 1) {
                dst[0] = dst[1] = dst[2] = src[0]; dst[3] = 255;
            } else if (n == 2) {
                dst[0] = dst[1] = dst[2] = src[0]; dst[3] = src[1];
            } else if (n == 3) {
                dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = 255;
            } else { // n == 4
                dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = src[3];
            }
        }
        outTextures.push_back(std::move(td));
    }
}

// Walk model.materials → Scene.materials. Also returns a flag per glTF image
// telling whether it should be treated as sRGB (baseColor + emissive) or
// linear (normal / metallicRoughness / occlusion). Same image referenced by
// two roles is rare; we choose sRGB by default and only flip to linear if
// the image is referenced as a linear-only role.
void extractMaterials(const tinygltf::Model& model,
                       std::vector<PbrMaterial>& outMats,
                       std::vector<bool>& outIsSRGB)
{
    outIsSRGB.assign(model.images.size(), true); // optimistic default

    auto markLinear = [&](int textureIdx) {
        if (textureIdx < 0
            || textureIdx >= static_cast<int>(model.textures.size())) return;
        int imgIdx = model.textures[textureIdx].source;
        if (imgIdx >= 0 && imgIdx < static_cast<int>(model.images.size())) {
            outIsSRGB[imgIdx] = false;
        }
    };

    outMats.clear();
    outMats.reserve(model.materials.size());
    for (size_t i = 0; i < model.materials.size(); ++i) {
        const auto& m = model.materials[i];
        PbrMaterial pm;
        pm.name = m.name.empty() ? ("material_" + std::to_string(i)) : m.name;

        const auto& pbr = m.pbrMetallicRoughness;
        if (pbr.baseColorFactor.size() == 4) {
            pm.baseColorFactor = glm::vec4(
                pbr.baseColorFactor[0], pbr.baseColorFactor[1],
                pbr.baseColorFactor[2], pbr.baseColorFactor[3]);
        }
        pm.metallicFactor  = static_cast<float>(pbr.metallicFactor);
        pm.roughnessFactor = static_cast<float>(pbr.roughnessFactor);

        if (m.emissiveFactor.size() == 3) {
            pm.emissiveFactor = glm::vec3(
                m.emissiveFactor[0], m.emissiveFactor[1], m.emissiveFactor[2]);
        }
        pm.normalScale       = static_cast<float>(m.normalTexture.scale);
        pm.occlusionStrength = static_cast<float>(m.occlusionTexture.strength);

        pm.baseColorTex         = pbr.baseColorTexture.index;
        pm.metallicRoughnessTex = pbr.metallicRoughnessTexture.index;
        pm.normalTex            = m.normalTexture.index;
        pm.emissiveTex          = m.emissiveTexture.index;
        pm.occlusionTex         = m.occlusionTexture.index;

        // Mark linear-role textures.
        markLinear(pm.metallicRoughnessTex);
        markLinear(pm.normalTex);
        markLinear(pm.occlusionTex);
        // baseColorTex and emissiveTex stay sRGB by default.

        outMats.push_back(std::move(pm));
    }
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

            // Materials first (we need to mark images as sRGB/linear before
            // texture extraction). Then images. Then walk meshes.
            std::vector<bool> isSRGB;
            extractMaterials(model, scene.materials, isSRGB);
            extractTextures(model, scene.textures, isSRGB);
            std::printf("[Scene] Loaded %zu materials, %zu textures.\n",
                         scene.materials.size(), scene.textures.size());
            std::fflush(stdout);

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
    // ============================================================
    // Optional xatlas regeneration of SyphonUV.
    //
    // If the scene folder contains uv1_atlas.bin, load it and replace the
    // mesh's positions/normals/uvs/uvs1/indices with the xatlas-regenerated
    // version. xatlas may split vertices at chart seams, so this is a
    // full mesh swap — but the surface (positions + topology projected to
    // 3D) is identical to the input.
    //
    // The cache is invalidated by mesh fingerprint (FNV-1a over positions
    // + indices). To force a regen, delete the file. The engine doesn't
    // run xatlas at load time automatically — that's an explicit operator
    // action from the UV Analysis panel (writes the file when done).
    // ============================================================
    {
        std::filesystem::path cachePath =
            std::filesystem::path(folderPath) / "uv1_atlas.bin";
        if (std::filesystem::exists(cachePath) && !scene.meshes.empty()) {
            uint64_t fp = meshFingerprint(scene.meshes[0]);
            UvAtlasResult atlas;
            if (loadAtlasCache(cachePath.string(), fp, atlas)) {
                MeshData& m = scene.meshes[0];
                // Cache format detection:
                //   - Legacy full-mesh cache: positions/indices populated;
                //     swap the whole mesh.
                //   - Proxy-pipeline cache: positions empty, uvs1 sized to
                //     dense.positions.size() — only update uvs1 on the
                //     already-loaded dense mesh.
                //   - Corrupt or stale: positions empty AND uvs1 wrong
                //     size → ignore.
                bool isProxyCache = atlas.positions.empty();
                bool uvSizeOk     = atlas.uvs1.size() == m.positions.size();
                if (isProxyCache && uvSizeOk) {
                    m.uvs1 = std::move(atlas.uvs1);
                    scene.atlasApplied = true;
                    std::printf("[Scene] xatlas cache (proxy/uvs1-only) loaded: "
                                 "%zu dense uvs1 applied, atlas %dx%d, %d charts\n",
                                 m.uvs1.size(), atlas.atlasWidth, atlas.atlasHeight,
                                 atlas.chartCount);
                } else if (!isProxyCache
                           && atlas.positions.size() > 0
                           && atlas.indices.size() > 0) {
                    std::printf("[Scene] xatlas cache (legacy full-mesh) loaded: "
                                 "%zu → %zu verts, %zu indices, %d charts, atlas %dx%d\n",
                                 m.positions.size(), atlas.positions.size(),
                                 atlas.indices.size(), atlas.chartCount,
                                 atlas.atlasWidth, atlas.atlasHeight);
                    m.positions = std::move(atlas.positions);
                    m.normals   = std::move(atlas.normals);
                    m.uvs       = std::move(atlas.uvs0);
                    m.uvs1      = std::move(atlas.uvs1);
                    m.indices   = std::move(atlas.indices);
                    scene.atlasApplied = true;
                } else {
                    std::printf("[Scene] xatlas cache loaded but had zero "
                                 "vertices or uv-size mismatch — ignoring.\n");
                }
            } else {
                std::printf("[Scene] xatlas cache exists but couldn't load "
                             "(stale or corrupt). Ignoring; mesh keeps glTF UVs.\n");
            }
        }
    }

    // Aggregate world-space bbox + centroid across all loaded meshes.
    // We apply each mesh's `transform` to its local positions on the fly
    // rather than re-baking them; transforms are usually identity for the
    // SpaceGen Blender export but we don't want to assume it.
    if (!scene.meshes.empty()) {
        glm::vec3 mn( std::numeric_limits<float>::max());
        glm::vec3 mx(-std::numeric_limits<float>::max());
        for (const auto& m : scene.meshes) {
            for (const auto& p : m.positions) {
                glm::vec4 w = m.transform * glm::vec4(p, 1.0f);
                mn = glm::min(mn, glm::vec3(w));
                mx = glm::max(mx, glm::vec3(w));
            }
        }
        scene.bboxMin  = mn;
        scene.bboxMax  = mx;
        scene.centroid = (mn + mx) * 0.5f;
        std::printf("[Scene] World bbox: (%.2f, %.2f, %.2f) -> (%.2f, %.2f, %.2f), "
                     "centroid (%.2f, %.2f, %.2f), dims (%.2f, %.2f, %.2f)\n",
                     mn.x, mn.y, mn.z, mx.x, mx.y, mx.z,
                     scene.centroid.x, scene.centroid.y, scene.centroid.z,
                     mx.x - mn.x, mx.y - mn.y, mx.z - mn.z);
    }

    std::printf("[Scene] loadFromFolder returning. %zu meshes in Scene.\n",
                 scene.meshes.size());
    std::fflush(stdout);
    return scene;
}

} // namespace spacegen
