#include "MeshDeformationLayer.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace spacegen {

namespace {

// Per-type display name used as the default `displayName` when an op is
// added. Operator can rename via the inspector.
const char* defaultOpName(DeformOpType t) {
    switch (t) {
        case DeformOpType::None:      return "None";
        case DeformOpType::CurlNoise: return "Curl Noise";
        case DeformOpType::Voronoi:   return "Voronoi";
        case DeformOpType::Twist:     return "Twist";
        case DeformOpType::Bend:      return "Bend";
        case DeformOpType::Taper:     return "Taper";
        case DeformOpType::Wave:      return "Wave";
        case DeformOpType::Spherify:  return "Spherify";
        case DeformOpType::FFDLite:   return "FFD-lite";
    }
    return "?";
}

// Set sensible defaults for a freshly-added op of a given type.
// The pA/pB layout for each op is documented in deformers.metal.inc.
void resetOpDefaults(DeformOp& op, DeformOpType type) {
    op.type        = type;
    op.enabled     = true;
    op.intensity   = 1.0f;
    op.pA          = glm::vec4(0.0f);
    op.pB          = glm::vec4(0.0f);
    op.intensityModSlot  = 0;
    op.intensityModDepth = 0.5f;

    switch (type) {
        case DeformOpType::CurlNoise:
            // pA.xyz = (scale, amount, speed); pA.w = time-evolve enable
            op.pA = glm::vec4(1.5f, 0.20f, 0.4f, 1.0f);
            break;
        case DeformOpType::Voronoi:
            // pA.xyz = (scale, shatter, jitter)
            op.pA = glm::vec4(1.0f, 0.30f, 1.0f, 0.0f);
            break;
        case DeformOpType::Twist:
            // pA.xyz = axis (unit), pA.w = amount (rad / m)
            // pB.xyz = center (m)
            op.pA = glm::vec4(0.0f, 0.0f, 1.0f, 0.8f);
            op.pB = glm::vec4(0.0f);
            break;
        case DeformOpType::Bend:
            // pA.xyz = axis, pA.w = angle (rad / m)
            // pB.xyz = bend direction (perpendicular to axis)
            op.pA = glm::vec4(0.0f, 0.0f, 1.0f, 0.6f);
            op.pB = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
            break;
        case DeformOpType::Taper:
            // pA.xyz = axis, pA.w = amount (1/m)
            // pB.xyz = center, pB.w = clamp (>0 -> clamp s ≥ 0)
            op.pA = glm::vec4(0.0f, 0.0f, 1.0f, -0.4f);
            op.pB = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
            break;
        case DeformOpType::Wave:
            // pA.xyz = dir, pA.w = freq (1/m)
            // pB.x = speed (Hz), pB.y = amount (m), pB.z = displaceAlong (0/1/2)
            op.pA = glm::vec4(1.0f, 0.0f, 0.0f, 2.0f);
            op.pB = glm::vec4(0.5f, 0.10f, 0.0f, 0.0f);
            break;
        case DeformOpType::Spherify:
            // pA.xyz = center, pA.w = radius
            // pB.x = amount (0..1; intensity also blends, this is per-op shape)
            op.pA = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
            op.pB = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
            break;
        case DeformOpType::FFDLite:
            // pA.xyz = bboxMin, pB.xyz = bboxMax; corners default to 0
            // (identity).
            op.pA = glm::vec4(-1.0f, -1.0f, -1.0f, 0.0f);
            op.pB = glm::vec4( 1.0f,  1.0f,  1.0f, 0.0f);
            for (auto& c : op.ffdCorners) c = glm::vec3(0.0f);
            break;
        default:
            break;
    }

    std::strncpy(op.displayName, defaultOpName(type),
                  sizeof(op.displayName) - 1);
    op.displayName[sizeof(op.displayName) - 1] = '\0';
}

const char* kModNames[] = {
    "None", "LFO 1", "LFO 2", "LFO 3", "LFO 4",
    "LFO 5", "LFO 6", "LFO 7", "LFO 8"
};

// Used by the "+ Add op" button — combo of all addable op types.
const std::pair<const char*, DeformOpType> kAddableOps[] = {
    {"Curl Noise", DeformOpType::CurlNoise},
    {"Voronoi",    DeformOpType::Voronoi},
    {"Twist",      DeformOpType::Twist},
    {"Bend",       DeformOpType::Bend},
    {"Taper",      DeformOpType::Taper},
    {"Wave",       DeformOpType::Wave},
    {"Spherify",   DeformOpType::Spherify},
    {"FFD-lite",   DeformOpType::FFDLite},
};

} // namespace

// ----------------------------------------------------------------------------
// Layer construction.
// ----------------------------------------------------------------------------

MeshDeformationLayer::MeshDeformationLayer() {
    name      = "Mesh Deformation";
    blendMode = BlendMode::Normal;
    colorTag  = glm::vec3(0.55f, 0.45f, 0.85f);
}

// ----------------------------------------------------------------------------
// Chain mutation.
// ----------------------------------------------------------------------------

DeformOp& MeshDeformationLayer::addOp(DeformOpType type) {
    if (static_cast<int>(ops.size()) >= kMaxDeformOps) {
        // Saturate at the cap. Return the last entry — the operator
        // will see the chain didn't grow and can remove one before
        // adding again.
        return ops.back();
    }
    ops.emplace_back();
    DeformOp& op = ops.back();
    resetOpDefaults(op, type);
    opIds_[ops.size() - 1] = nextOpId_++;
    return op;
}

void MeshDeformationLayer::removeOp(int idx) {
    if (idx < 0 || idx >= static_cast<int>(ops.size())) return;
    ops.erase(ops.begin() + idx);
    // Shift the id table left so the ImGui IDs stay attached to their
    // respective ops post-deletion.
    for (int i = idx; i + 1 < kMaxDeformOps; ++i) {
        opIds_[i] = opIds_[i + 1];
    }
    opIds_[kMaxDeformOps - 1] = 0;
}

void MeshDeformationLayer::moveOp(int from, int to) {
    const int n = static_cast<int>(ops.size());
    if (n <= 1) return;
    if (from < 0 || from >= n) return;
    if (to   < 0) to = 0;
    if (to   >= n) to = n - 1;
    if (from == to) return;
    DeformOp tmp = ops[from];
    uint32_t tmpId = opIds_[from];
    ops.erase(ops.begin() + from);
    opIds_[from] = 0;  // will be re-shifted below
    ops.insert(ops.begin() + to, tmp);
    // Re-shift the id table to match. Simplest: rebuild from a
    // working copy.
    std::array<uint32_t, kMaxDeformOps> newIds{};
    int k = 0;
    for (int i = 0; i < kMaxDeformOps && k < static_cast<int>(ops.size()); ++i) {
        if (i == from) continue;
        if (k == to) {
            newIds[k++] = tmpId;
            if (k >= static_cast<int>(ops.size())) break;
        }
        newIds[k++] = opIds_[i];
    }
    if (k == to && k < static_cast<int>(ops.size())) {
        newIds[k] = tmpId;
    }
    opIds_ = newIds;
}

// ----------------------------------------------------------------------------
// CPU -> GPU snapshot.
// ----------------------------------------------------------------------------

MeshDeformationLayer::GpuChain
MeshDeformationLayer::snapshot(double t, const ModulatorBank* mods) const
{
    GpuChain chain{};
    int outIdx  = 0;
    int ffdSlot = 0;

    for (const auto& op : ops) {
        if (!op.enabled) continue;
        if (op.type == DeformOpType::None) continue;

        // Effective intensity = base + bank * depth, clamped to [0, 1].
        float wet = op.intensity;
        if (mods && op.intensityModSlot > 0) {
            wet += mods->eval(op.intensityModSlot, t) * op.intensityModDepth;
        }
        wet = std::clamp(wet, 0.0f, 1.0f);
        if (wet <= 0.0f) continue;

        if (outIdx >= kMaxDeformOps) break;

        auto& g = chain.ops[outIdx];
        // Header layout:
        //   .x = type as int
        //   .y = effective intensity
        //   .z = time (so ops that need 't' don't need a second uniform)
        //   .w = FFD slot index (only set by FFD ops; -1 otherwise)
        g.header = glm::vec4(static_cast<float>(static_cast<int>(op.type)),
                              wet,
                              static_cast<float>(t),
                              -1.0f);
        g.pA = op.pA;
        g.pB = op.pB;

        if (op.type == DeformOpType::FFDLite) {
            if (ffdSlot < GpuChain::kMaxFfdSlots) {
                auto& slot = chain.ffdSlots[ffdSlot];
                for (int c = 0; c < 8; ++c) {
                    slot[c] = glm::vec4(op.ffdCorners[c], 0.0f);
                }
                g.header.w = static_cast<float>(ffdSlot);
                ++ffdSlot;
            } else {
                // Out of FFD slots — degrade to identity by setting type
                // to None.
                g.header.x = static_cast<float>(static_cast<int>(DeformOpType::None));
            }
        }

        ++outIdx;
    }

    chain.count    = outIdx;
    chain.ffdCount = ffdSlot;
    return chain;
}

// ----------------------------------------------------------------------------
// ImGui inspector.
// ----------------------------------------------------------------------------

// Per-op inspector. Returns true if the operator clicked the remove
// button. Writes -1/+1 to *moveDir if reorder was requested.
bool MeshDeformationLayer::drawOpInspector(int idx, int* moveDir) {
    DeformOp& op = ops[static_cast<size_t>(idx)];
    bool removed = false;
    *moveDir = 0;

    ImGui::PushID(static_cast<int>(opIds_[idx] ? opIds_[idx] : idx + 1));

    // Header row: enable toggle, name, up/down/remove buttons.
    ImGui::Checkbox("##en", &op.enabled);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160.0f);
    ImGui::InputText("##name", op.displayName, sizeof(op.displayName));
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", defaultOpName(op.type));

    // Right-aligned reorder + remove.
    ImGui::SameLine();
    float avail = ImGui::GetContentRegionAvail().x;
    ImGui::SameLine(ImGui::GetCursorPosX() + avail - 88.0f);
    if (ImGui::SmallButton("^")) *moveDir = -1;
    ImGui::SameLine();
    if (ImGui::SmallButton("v")) *moveDir = +1;
    ImGui::SameLine();
    if (ImGui::SmallButton("x")) removed = true;

    // Body: intensity + per-op params.
    if (op.enabled) {
        ImGui::Indent(12.0f);
        ImGui::SliderFloat("Intensity##i", &op.intensity, 0.0f, 1.0f);
        ImGui::SetNextItemWidth(90.0f);
        ImGui::Combo("##imod", &op.intensityModSlot, kModNames, 9);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        ImGui::SliderFloat("Mod depth##im",
                            &op.intensityModDepth, 0.0f, 2.0f);

        switch (op.type) {
            case DeformOpType::CurlNoise:
                ImGui::SliderFloat("Scale##s",  &op.pA.x, 0.1f, 10.0f);
                ImGui::SliderFloat("Amount (m)##a", &op.pA.y, 0.0f, 1.0f);
                ImGui::SliderFloat("Speed (Hz)##sp", &op.pA.z, 0.0f, 4.0f);
                ImGui::Checkbox("Time-evolve##te",
                                  reinterpret_cast<bool*>(&op.pA.w));
                break;

            case DeformOpType::Voronoi:
                ImGui::SliderFloat("Scale##s",   &op.pA.x, 0.1f, 10.0f);
                ImGui::SliderFloat("Shatter##sh",&op.pA.y, -1.0f, 1.0f);
                ImGui::SliderFloat("Jitter##j",  &op.pA.z, 0.0f, 1.0f);
                break;

            case DeformOpType::Twist:
                ImGui::SliderFloat3("Axis##ax", &op.pA.x, -1.0f, 1.0f);
                ImGui::SliderFloat("Amount (rad/m)##am",
                                    &op.pA.w, -3.0f, 3.0f);
                ImGui::SliderFloat3("Center##cn", &op.pB.x, -5.0f, 5.0f);
                if (ImGui::SmallButton("X")) op.pA = glm::vec4(1,0,0, op.pA.w);
                ImGui::SameLine();
                if (ImGui::SmallButton("Y")) op.pA = glm::vec4(0,1,0, op.pA.w);
                ImGui::SameLine();
                if (ImGui::SmallButton("Z")) op.pA = glm::vec4(0,0,1, op.pA.w);
                break;

            case DeformOpType::Bend:
                ImGui::SliderFloat3("Axis##ax", &op.pA.x, -1.0f, 1.0f);
                ImGui::SliderFloat("Angle (rad/m)##an",
                                    &op.pA.w, -2.0f, 2.0f);
                ImGui::SliderFloat3("Bend dir##bd",
                                     &op.pB.x, -1.0f, 1.0f);
                break;

            case DeformOpType::Taper:
                ImGui::SliderFloat3("Axis##ax", &op.pA.x, -1.0f, 1.0f);
                ImGui::SliderFloat("Amount (1/m)##am",
                                    &op.pA.w, -2.0f, 2.0f);
                ImGui::SliderFloat3("Center##cn",
                                     &op.pB.x, -5.0f, 5.0f);
                ImGui::Checkbox("Clamp (no inversion)##cl",
                                  reinterpret_cast<bool*>(&op.pB.w));
                break;

            case DeformOpType::Wave:
                ImGui::SliderFloat3("Dir##d", &op.pA.x, -1.0f, 1.0f);
                ImGui::SliderFloat("Freq (1/m)##fr",
                                    &op.pA.w, 0.0f, 10.0f);
                ImGui::SliderFloat("Speed (Hz)##sp",
                                    &op.pB.x, 0.0f, 4.0f);
                ImGui::SliderFloat("Amount (m)##am",
                                    &op.pB.y, 0.0f, 1.0f);
                {
                    int along = static_cast<int>(op.pB.z + 0.5f);
                    const char* alongNames[] = {"Normal", "World up", "Wave dir"};
                    ImGui::Combo("Displace along##da",
                                  &along, alongNames, 3);
                    op.pB.z = static_cast<float>(along);
                }
                break;

            case DeformOpType::Spherify:
                ImGui::SliderFloat3("Center##cn", &op.pA.x, -5.0f, 5.0f);
                ImGui::SliderFloat("Radius##r",
                                    &op.pA.w, 0.1f, 10.0f);
                ImGui::SliderFloat("Strength##st",
                                    &op.pB.x, 0.0f, 1.0f);
                break;

            case DeformOpType::FFDLite:
                ImGui::SliderFloat3("BBox min##bmn", &op.pA.x, -10.0f, 10.0f);
                ImGui::SliderFloat3("BBox max##bmx", &op.pB.x, -10.0f, 10.0f);
                if (ImGui::TreeNode("8 corner deltas")) {
                    static const char* kCornerNames[8] = {
                        "(-,-,-)", "(+,-,-)", "(-,+,-)", "(+,+,-)",
                        "(-,-,+)", "(+,-,+)", "(-,+,+)", "(+,+,+)"
                    };
                    for (int c = 0; c < 8; ++c) {
                        ImGui::PushID(c);
                        ImGui::TextDisabled("%s", kCornerNames[c]);
                        ImGui::SameLine(110.0f);
                        ImGui::SetNextItemWidth(220.0f);
                        ImGui::SliderFloat3("##d",
                                             &op.ffdCorners[c].x,
                                             -3.0f, 3.0f);
                        ImGui::PopID();
                    }
                    if (ImGui::SmallButton("Reset corners")) {
                        for (auto& c : op.ffdCorners) c = glm::vec3(0.0f);
                    }
                    ImGui::TreePop();
                }
                break;

            default:
                ImGui::TextDisabled("(unknown op type)");
                break;
        }
        ImGui::Unindent(12.0f);
    }

    ImGui::PopID();
    return removed;
}

void MeshDeformationLayer::drawInspector() {
    if (ImGui::CollapsingHeader("Deformer chain",
                                  ImGuiTreeNodeFlags_DefaultOpen)) {
        // Chain summary.
        ImGui::Text("%zu / %d ops", ops.size(),
                     static_cast<int>(kMaxDeformOps));
        ImGui::SameLine();
        ImGui::TextDisabled("(applied top -> bottom)");

        // Per-op inspectors.
        int toRemove   = -1;
        int moveSrc    = -1;
        int moveTarget = -1;
        for (int i = 0; i < static_cast<int>(ops.size()); ++i) {
            ImGui::Separator();
            int dir = 0;
            if (drawOpInspector(i, &dir)) {
                toRemove = i;
            }
            if (dir != 0) {
                moveSrc    = i;
                moveTarget = i + dir;
            }
        }
        if (toRemove >= 0) {
            removeOp(toRemove);
        } else if (moveSrc >= 0) {
            moveOp(moveSrc, moveTarget);
        }

        ImGui::Separator();

        // Add-op row.
        static int addTypeIdx = 0;
        const int kAddableCount =
            sizeof(kAddableOps) / sizeof(kAddableOps[0]);
        const char* items[kAddableCount];
        for (int i = 0; i < kAddableCount; ++i) {
            items[i] = kAddableOps[i].first;
        }
        ImGui::SetNextItemWidth(160.0f);
        ImGui::Combo("##addType", &addTypeIdx, items, kAddableCount);
        ImGui::SameLine();
        if (ImGui::Button("+ Add op")) {
            if (static_cast<int>(ops.size()) < kMaxDeformOps) {
                addOp(kAddableOps[addTypeIdx].second);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear all") && !ops.empty()) {
            ops.clear();
            opIds_.fill(0);
        }
    }

    ImGui::TextDisabled("Vertex-shader chain. Cost ~30 ALU + 1-3 noise");
    ImGui::TextDisabled("lookups per active op per vertex.");
}

} // namespace spacegen
