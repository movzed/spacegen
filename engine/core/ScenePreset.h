#pragma once
// ScenePreset — save / load the layer STACK of a Bus to a JSON file.
//
// Motivation: the operator builds a layer stack (lights, materials, FX) in the
// Layer Rack every launch from scratch. A preset captures that stack so it can
// be reloaded — and one preset can be marked the startup default so the rack
// comes back automatically.
//
// HEADER-ONLY (inline). Reuses nlohmann/json (already a dependency of
// Workstation.mm) and the LayerFactory to rebuild concrete layers from their
// typeName.
//
// WHAT IS SERIALIZED, per layer:
//   - typeName  (REQUIRED — drives makeLayerByTypeName on load)
//   - name, state (enum int), opacity, blendMode (enum int), colorTag (rgb)
//   - PLUS the high-value params for the common layer types, via a per-type
//     switch on typeName (lights' color / intensity / pan / tilt; structure's
//     baseColor / roughness / metallic; effect layers keep the base opacity
//     which is their master mix). Layers not covered still round-trip their
//     stack position + enable/opacity/blend — the PRIMARY win.
//
// EXTENSION POINT: to persist more fields for a type, add a branch in
// writeLayerParams() / readLayerParams() below. Nothing else changes.

#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "Layer.h"
#include "LayerFactory.h"

// Concrete headers for the param dynamic_casts. (All already included
// transitively via LayerFactory.h, but listed for clarity of intent.)
#include "StructureLayer.h"
#include "BeamLayer.h"
#include "DirectionalLightLayer.h"
#include "AmbientLightLayer.h"
#include "SyphonInputLayer.h"
#include "../effects/area_light/AreaLightLayer.h"
#include "../effects/light_cloner/LightClonerLayer.h"

namespace spacegen {

namespace detail {

inline nlohmann::json vec3ToJson(const glm::vec3& v) {
    return nlohmann::json::array({ v.x, v.y, v.z });
}

inline glm::vec3 jsonToVec3(const nlohmann::json& j, const glm::vec3& def) {
    if (!j.is_array() || j.size() < 3) return def;
    return glm::vec3(j[0].get<float>(), j[1].get<float>(), j[2].get<float>());
}

// ---- Per-type param serialization (extension point) -----------------------

inline void writeLayerParams(const ILayer* l, nlohmann::json& jp) {
    if (auto* s = dynamic_cast<const StructureLayer*>(l)) {
        jp["baseColor"] = vec3ToJson(s->baseColor);
        jp["roughness"] = s->roughness;
        jp["metallic"]  = s->metallic;
        return;
    }
    if (auto* b = dynamic_cast<const BeamLayer*>(l)) {
        jp["color"]        = vec3ToJson(b->color);
        jp["intensity"]    = b->intensity;
        jp["panDeg"]       = b->panDeg;
        jp["tiltDeg"]      = b->tiltDeg;
        jp["range"]        = b->range;
        jp["innerDeg"]     = b->innerDeg;
        jp["outerDeg"]     = b->outerDeg;
        jp["followCamera"] = b->followCamera;
        return;
    }
    if (auto* d = dynamic_cast<const DirectionalLightLayer*>(l)) {
        jp["color"]     = vec3ToJson(d->color);
        jp["intensity"] = d->intensity;
        jp["panDeg"]    = d->panDeg;
        jp["tiltDeg"]   = d->tiltDeg;
        return;
    }
    if (auto* a = dynamic_cast<const AmbientLightLayer*>(l)) {
        jp["color"]     = vec3ToJson(a->color);
        jp["intensity"] = a->intensity;
        return;
    }
    if (auto* a = dynamic_cast<const AreaLightLayer*>(l)) {
        jp["color"]     = vec3ToJson(a->color);
        jp["intensity"] = a->intensity;
        jp["panDeg"]    = a->panDeg;
        jp["tiltDeg"]   = a->tiltDeg;
        jp["range"]     = a->range;
        return;
    }
    if (auto* c = dynamic_cast<const LightClonerLayer*>(l)) {
        jp["clones"]     = c->clones;
        jp["colorStart"] = vec3ToJson(c->colorStart);
        jp["colorEnd"]   = vec3ToJson(c->colorEnd);
        return;
    }
    // Other layer types: only the common envelope (state/opacity/blend/tag)
    // round-trips. opacity IS the master mix for the effect layers, so the
    // primary win — stack + enable/opacity — already works for them.
}

inline void readLayerParams(ILayer* l, const nlohmann::json& jp) {
    if (auto* s = dynamic_cast<StructureLayer*>(l)) {
        if (jp.contains("baseColor")) s->baseColor = jsonToVec3(jp["baseColor"], s->baseColor);
        if (jp.contains("roughness")) s->roughness = jp["roughness"].get<float>();
        if (jp.contains("metallic"))  s->metallic  = jp["metallic"].get<float>();
        return;
    }
    if (auto* b = dynamic_cast<BeamLayer*>(l)) {
        if (jp.contains("color"))        b->color        = jsonToVec3(jp["color"], b->color);
        if (jp.contains("intensity"))    b->intensity    = jp["intensity"].get<float>();
        if (jp.contains("panDeg"))       b->panDeg       = jp["panDeg"].get<float>();
        if (jp.contains("tiltDeg"))      b->tiltDeg      = jp["tiltDeg"].get<float>();
        if (jp.contains("range"))        b->range        = jp["range"].get<float>();
        if (jp.contains("innerDeg"))     b->innerDeg     = jp["innerDeg"].get<float>();
        if (jp.contains("outerDeg"))     b->outerDeg     = jp["outerDeg"].get<float>();
        if (jp.contains("followCamera")) b->followCamera = jp["followCamera"].get<bool>();
        return;
    }
    if (auto* d = dynamic_cast<DirectionalLightLayer*>(l)) {
        if (jp.contains("color"))     d->color     = jsonToVec3(jp["color"], d->color);
        if (jp.contains("intensity")) d->intensity = jp["intensity"].get<float>();
        if (jp.contains("panDeg"))    d->panDeg    = jp["panDeg"].get<float>();
        if (jp.contains("tiltDeg"))   d->tiltDeg   = jp["tiltDeg"].get<float>();
        return;
    }
    if (auto* a = dynamic_cast<AmbientLightLayer*>(l)) {
        if (jp.contains("color"))     a->color     = jsonToVec3(jp["color"], a->color);
        if (jp.contains("intensity")) a->intensity = jp["intensity"].get<float>();
        return;
    }
    if (auto* a = dynamic_cast<AreaLightLayer*>(l)) {
        if (jp.contains("color"))     a->color     = jsonToVec3(jp["color"], a->color);
        if (jp.contains("intensity")) a->intensity = jp["intensity"].get<float>();
        if (jp.contains("panDeg"))    a->panDeg    = jp["panDeg"].get<float>();
        if (jp.contains("tiltDeg"))   a->tiltDeg   = jp["tiltDeg"].get<float>();
        if (jp.contains("range"))     a->range     = jp["range"].get<float>();
        return;
    }
    if (auto* c = dynamic_cast<LightClonerLayer*>(l)) {
        if (jp.contains("clones"))     c->clones     = jp["clones"].get<int>();
        if (jp.contains("colorStart")) c->colorStart = jsonToVec3(jp["colorStart"], c->colorStart);
        if (jp.contains("colorEnd"))   c->colorEnd   = jsonToVec3(jp["colorEnd"], c->colorEnd);
        return;
    }
}

} // namespace detail

// Serialize the bus's layer stack to `path`. Returns true on success.
inline bool savePreset(const Bus& bus, const std::string& path) {
    using json = nlohmann::json;
    json j;
    j["version"] = 1;
    j["bus"]     = bus.name;
    json layers  = json::array();

    for (const auto& l : bus.layers) {
        if (!l) continue;
        json jl;
        jl["typeName"]  = l->typeName();                  // REQUIRED
        jl["name"]      = l->name;
        jl["state"]     = static_cast<int>(l->state);
        jl["opacity"]   = l->opacity;
        jl["blendMode"] = static_cast<int>(l->blendMode);
        jl["colorTag"]  = detail::vec3ToJson(l->colorTag);

        json jp = json::object();
        detail::writeLayerParams(l.get(), jp);
        if (!jp.empty()) jl["params"] = jp;

        layers.push_back(std::move(jl));
    }
    j["layers"] = std::move(layers);

    std::ofstream f(path);
    if (!f) return false;
    f << j.dump(2);
    return true;
}

// Rebuild the bus's layer stack from `path`. CLEARS the existing stack first,
// then constructs each layer via the factory and restores its envelope +
// params. Returns true if the file was read and applied. Unknown typeNames are
// skipped. On any read/parse failure the bus is left UNTOUCHED (returns false).
inline bool loadPreset(Bus& bus, const std::string& path) {
    using json = nlohmann::json;
    std::ifstream f(path);
    if (!f) return false;
    json j;
    try { f >> j; } catch (...) { return false; }
    if (!j.contains("layers") || !j["layers"].is_array()) return false;

    // Build into a temp vector first so a malformed entry mid-file can't leave
    // the live bus half-cleared.
    std::vector<std::unique_ptr<ILayer>> rebuilt;
    rebuilt.reserve(j["layers"].size());

    for (const auto& jl : j["layers"]) {
        if (!jl.contains("typeName")) continue;
        std::string type = jl["typeName"].get<std::string>();
        auto layer = makeLayerByTypeName(type);
        if (!layer) continue;  // unknown / unavailable type — skip

        if (jl.contains("name"))    layer->name    = jl["name"].get<std::string>();
        if (jl.contains("opacity")) layer->opacity = jl["opacity"].get<float>();
        if (jl.contains("state"))
            layer->state = static_cast<LayerState>(jl["state"].get<int>());
        if (jl.contains("blendMode"))
            layer->blendMode = static_cast<BlendMode>(jl["blendMode"].get<int>());
        if (jl.contains("colorTag"))
            layer->colorTag = detail::jsonToVec3(jl["colorTag"], layer->colorTag);
        if (jl.contains("params"))
            detail::readLayerParams(layer.get(), jl["params"]);

        rebuilt.push_back(std::move(layer));
    }

    bus.layers = std::move(rebuilt);
    if (j.contains("bus")) bus.name = j["bus"].get<std::string>();
    return true;
}

} // namespace spacegen
