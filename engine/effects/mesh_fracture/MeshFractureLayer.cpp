#include "MeshFractureLayer.h"

#include "../../core/ModulatorBank.h"

#include <algorithm>
#include <cmath>

#include "imgui.h"

namespace spacegen {

namespace {

constexpr double kTwoPi = 6.283185307179586476925287;

// Hermite smoothstep — zero-derivative at both ends, so ramps ease in *and*
// out without a visible kink. This is what gives dissolve/cracks the
// "animacion suave" the user asked for.
inline float smoothstep01(float x) {
    x = std::clamp(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

// Eased attack -> hold -> decay envelope, normalized to [0,1].
//   local : seconds since the envelope started (>= 0)
//   a/h/d : attack, hold, decay durations (s)
// Returns 0 before/after the active window (the caller decides whether to
// loop). Attack and decay are smoothstepped; hold sits flat at 1.
inline float ahdEnvelope(double local, float a, float h, float d) {
    if (local < 0.0) return 0.0f;
    const double tA = std::max(0.0f, a);
    const double tH = std::max(0.0f, h);
    const double tD = std::max(0.0f, d);
    if (local < tA) {
        return tA > 1e-6 ? smoothstep01(float(local / tA)) : 1.0f;
    }
    if (local < tA + tH) {
        return 1.0f;
    }
    const double into = local - (tA + tH);
    if (into < tD) {
        return tD > 1e-6 ? (1.0f - smoothstep01(float(into / tD))) : 0.0f;
    }
    return 0.0f;
}

} // namespace

MeshFractureLayer::MeshFractureLayer() {
    name      = "Mesh Fracture";
    blendMode = BlendMode::Normal;
    colorTag  = glm::vec3(0.92f, 0.42f, 0.18f);  // burnt orange
}

// The single resolved value the renderer reads. Built-in animation is folded
// in here on top of the static amount + modulator bank, then clamped — no new
// GPU-facing fields are introduced.
float MeshFractureLayer::effectiveAmount(double t,
                                        const ModulatorBank* mods) const {
    float a = amount;
    if (mods) {
        a += mods->eval(amountModSlot, t) * amountModDepth;
    }

    // Built-in smooth envelope (added on top of static + modulator).
    const float depth = std::clamp(animDepth, 0.0f, 1.0f);
    const float floor = std::clamp(animFloor, 0.0f, 1.0f);
    switch (animMode) {
        case AnimMode::Static:
            break;

        case AnimMode::Pulse: {
            // Cosine breathing in [floor .. floor+depth]. Cosine is already
            // C1-smooth, so no extra easing needed.
            const double phase = t * double(animRateHz) * kTwoPi;
            const float  s     = 0.5f * (1.0f - std::cos(float(phase))); // 0..1
            a += floor + depth * s;
            break;
        }

        case AnimMode::Loop: {
            // Repeating eased attack->hold->decay over a 1/rate window.
            const double period = animRateHz > 1e-4f ? 1.0 / double(animRateHz)
                                                      : 1e9;
            const double local  = std::fmod(t, period);
            a += floor + depth * ahdEnvelope(local, animAttack, animHold, animDecay);
            break;
        }

        case AnimMode::OneShot: {
            double start = animTriggerTime;
            if (animLoopOneShot) {
                // Auto-refire every animPeriod seconds, phase-locked to the
                // global clock so it's deterministic across reloads.
                const double per = std::max(0.05f, animPeriod);
                start = std::floor(t / per) * per;
            }
            if (start >= 0.0 && t >= start) {
                const double local = t - start;
                a += floor + depth * ahdEnvelope(local, animAttack, animHold, animDecay);
            } else if (start >= 0.0) {
                a += floor; // armed, pre-fire
            }
            break;
        }
    }

    return std::clamp(a, 0.0f, 1.0f);
}

bool MeshFractureLayer::active(double t, const ModulatorBank* mods) const {
    if (state != LayerState::Enabled) return false;
    if (mode == 0)                    return false;
    return effectiveAmount(t, mods) > 1e-4f;
}

void MeshFractureLayer::applyPreset(Preset p, double now) {
    currentPreset = p;
    switch (p) {
        case Preset::Custom:
            return;

        case Preset::SlowDissolve: {
            // A slab that keeps melting and re-forming from a center point.
            mode = ModeDissolve;
            amount            = 0.0f;             // envelope drives it
            useRadialDissolve = true;
            burnPoint         = glm::vec3(0.0f);
            dissolveScale     = 2.0f;
            dissolveSpeed     = 0.08f;            // gentle threshold drift
            dissolveRadius    = 14.0f;
            dissolveRadialBias = 0.65f;
            dissolveEdgeWidth = 0.09f;
            dissolveEdgeGlow  = 1.8f;
            dissolveEdgeColor = glm::vec3(1.00f, 0.55f, 0.18f); // warm amber
            // Slow eased loop — long rise/fall so it reads as a melt, not a blink.
            animMode   = AnimMode::Loop;
            animRateHz = 0.06f;                   // ~16 s per cycle
            animDepth  = 0.85f;
            animFloor  = 0.05f;
            animAttack = 6.0f;
            animHold   = 2.0f;
            animDecay  = 8.0f;
            break;
        }

        case Preset::CrackPulse: {
            // An ember crack network that breathes — glows up, settles, repeats.
            mode = ModeCracks;
            amount          = 0.0f;
            crackDensity    = 2.2f;
            crackWidth      = 0.05f;
            crackJitter     = 0.75f;
            crackJitterSpeed = 0.10f;
            crackDarken     = 0.80f;
            crackGlowColor  = glm::vec3(1.00f, 0.50f, 0.16f); // ember
            crackGlow       = 1.6f;
            animMode   = AnimMode::Pulse;
            animRateHz = 0.30f;                   // slow tasteful breathing
            animDepth  = 0.70f;
            animFloor  = 0.15f;                   // cracks never fully vanish
            break;
        }

        case Preset::ExplodeBurst: {
            // Shards part outward once, then ease back. Tuned to read as
            // breakage, not noise — moderate density, balanced jitter+spin,
            // strength near the renderer's 0.5 m cap.
            mode = ModeExplode;
            amount          = 0.0f;
            shardDensity    = 2.4f;
            shardJitter     = 0.40f;
            shardSpin       = 0.55f;
            explodeStrength = 0.48f;              // just under the 0.5 m cap
            animMode   = AnimMode::OneShot;
            animDepth  = 1.0f;
            animFloor  = 0.0f;
            animAttack = 0.18f;                   // snap apart
            animHold   = 0.25f;
            animDecay  = 1.20f;                   // settle back gracefully
            animLoopOneShot = false;
            retrigger(now);                       // fire immediately on apply
            break;
        }

        case Preset::GlitchStorm: {
            // Aggressive datamosh — fast bands, frequent re-roll, fast pulse.
            mode = ModeGlitch;
            amount          = 0.0f;
            glitchFrequency = 9.0f;
            glitchMagnitude = 0.55f;
            glitchSpeed     = 14.0f;
            glitchDropout   = 0.22f;
            animMode   = AnimMode::Pulse;
            animRateHz = 2.5f;                    // rapid stutter
            animDepth  = 0.85f;
            animFloor  = 0.10f;
            break;
        }
    }
}

void MeshFractureLayer::drawInspector() {
    static const char* kModNames[] = {
        "None", "LFO 1", "LFO 2", "LFO 3", "LFO 4",
        "LFO 5", "LFO 6", "LFO 7", "LFO 8"
    };

    // ---- Movement presets (apply on select) --------------------------------
    if (ImGui::CollapsingHeader("Preset", ImGuiTreeNodeFlags_DefaultOpen)) {
        static const char* kPresetNames[] = {
            "Custom", "Slow Dissolve", "Crack Pulse",
            "Explode Burst", "Glitch Storm"
        };
        int sel = static_cast<int>(currentPreset);
        ImGui::SetNextItemWidth(180.0f);
        if (ImGui::Combo("##fracpreset", &sel, kPresetNames,
                         IM_ARRAYSIZE(kPresetNames))) {
            applyPreset(static_cast<Preset>(sel), ImGui::GetTime());
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("One-click movement recipes. Editing any control\n"
                              "below keeps the values but the dropdown reads\n"
                              "'Custom' until you pick a preset again.");
        }
    }

    if (ImGui::CollapsingHeader("Mode", ImGuiTreeNodeFlags_DefaultOpen)) {
        bool c = (mode & ModeCracks)   != 0;
        bool d = (mode & ModeDissolve) != 0;
        bool e = (mode & ModeExplode)  != 0;
        bool g = (mode & ModeGlitch)   != 0;
        bool changed = false;
        if (ImGui::Checkbox("Animated cracks##frc",   &c)) { mode = c ? (mode | ModeCracks)   : (mode & ~ModeCracks);   changed = true; }
        if (ImGui::Checkbox("Dissolve burn##frd",     &d)) { mode = d ? (mode | ModeDissolve) : (mode & ~ModeDissolve); changed = true; }
        if (ImGui::Checkbox("Per-shard explode##fre", &e)) { mode = e ? (mode | ModeExplode)  : (mode & ~ModeExplode);  changed = true; }
        if (ImGui::Checkbox("Glitch tear##frg",       &g)) { mode = g ? (mode | ModeGlitch)   : (mode & ~ModeGlitch);   changed = true; }
        if (changed) currentPreset = Preset::Custom;
    }

    // ---- Built-in animation -------------------------------------------------
    if (ImGui::CollapsingHeader("Animation", ImGuiTreeNodeFlags_DefaultOpen)) {
        static const char* kAnimNames[] = { "Static", "Pulse", "Loop", "One-Shot" };
        int am = static_cast<int>(animMode);
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::Combo("Movement##fram", &am, kAnimNames, IM_ARRAYSIZE(kAnimNames))) {
            animMode = static_cast<AnimMode>(am);
            currentPreset = Preset::Custom;
        }

        if (animMode != AnimMode::Static) {
            ImGui::SliderFloat("Depth##fradp",  &animDepth, 0.0f, 1.0f);
            ImGui::SliderFloat("Floor##frafl",  &animFloor, 0.0f, 1.0f);
        }
        if (animMode == AnimMode::Pulse || animMode == AnimMode::Loop) {
            ImGui::SliderFloat("Rate (Hz)##frart", &animRateHz, 0.01f, 4.0f,
                               "%.3f", ImGuiSliderFlags_Logarithmic);
        }
        if (animMode == AnimMode::Loop || animMode == AnimMode::OneShot) {
            ImGui::SliderFloat("Attack (s)##fraat", &animAttack, 0.0f, 10.0f);
            ImGui::SliderFloat("Hold (s)##frahd",   &animHold,   0.0f, 10.0f);
            ImGui::SliderFloat("Decay (s)##fradc",  &animDecay,  0.0f, 10.0f);
        }
        if (animMode == AnimMode::OneShot) {
            if (ImGui::Button("Retrigger##frartg")) {
                retrigger(ImGui::GetTime());
            }
            ImGui::SameLine();
            ImGui::Checkbox("Auto loop##fralp", &animLoopOneShot);
            if (animLoopOneShot) {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(120.0f);
                ImGui::SliderFloat("Every (s)##frapr", &animPeriod, 0.1f, 20.0f);
            }
        }
    }

    if (ImGui::CollapsingHeader("Amount (master)",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        bool ed = false;
        ed |= ImGui::SliderFloat("Amount##frac", &amount, 0.0f, 1.0f);
        ImGui::SetNextItemWidth(90.0f);
        ed |= ImGui::Combo("##fracmod", &amountModSlot, kModNames, 9);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        ed |= ImGui::SliderFloat("Depth##fracmd", &amountModDepth, 0.0f, 2.0f);
        if (ed) currentPreset = Preset::Custom;
    }

    if (mode & ModeCracks) {
        if (ImGui::CollapsingHeader("Cracks",
                                    ImGuiTreeNodeFlags_DefaultOpen)) {
            bool ed = false;
            ed |= ImGui::SliderFloat("Density (c/m)##fcd", &crackDensity, 0.2f, 8.0f);
            ed |= ImGui::SliderFloat("Edge width##fcw",     &crackWidth,   0.005f, 0.30f);
            ed |= ImGui::SliderFloat("Cell jitter##fcj",    &crackJitter,  0.0f, 1.0f);
            ed |= ImGui::SliderFloat("Jitter speed##fcjs",  &crackJitterSpeed, 0.0f, 2.0f);
            ed |= ImGui::SliderFloat("Darken##fcd2",        &crackDarken,  0.0f, 1.0f);
            ed |= ImGui::ColorEdit3("Glow color##fcc",
                              &crackGlowColor[0],
                              ImGuiColorEditFlags_PickerHueWheel
                              | ImGuiColorEditFlags_Float);
            ed |= ImGui::SliderFloat("Glow##fcg", &crackGlow, 0.0f, 8.0f);
            if (ed) currentPreset = Preset::Custom;
        }
    }

    if (mode & ModeDissolve) {
        if (ImGui::CollapsingHeader("Dissolve burn",
                                    ImGuiTreeNodeFlags_DefaultOpen)) {
            bool ed = false;
            ed |= ImGui::Checkbox("Radial##fdr", &useRadialDissolve);
            ed |= ImGui::InputFloat3("Burn point##fdp", &burnPoint[0]);
            ed |= ImGui::SliderFloat("Noise scale##fdn", &dissolveScale, 0.1f, 12.0f);
            ed |= ImGui::SliderFloat("Speed##fdsp",      &dissolveSpeed, -1.0f, 1.0f);
            ed |= ImGui::SliderFloat("Radius (m)##fdrad", &dissolveRadius, 0.5f, 50.0f);
            ed |= ImGui::SliderFloat("Radial bias##fdrb", &dissolveRadialBias, 0.0f, 1.5f);
            ed |= ImGui::SliderFloat("Edge width##fdew",  &dissolveEdgeWidth, 0.005f, 0.30f);
            ed |= ImGui::SliderFloat("Edge glow##fdeg",   &dissolveEdgeGlow,  0.0f, 8.0f);
            ed |= ImGui::ColorEdit3("Edge color##fdec",
                              &dissolveEdgeColor[0],
                              ImGuiColorEditFlags_PickerHueWheel
                              | ImGuiColorEditFlags_Float);
            if (ed) currentPreset = Preset::Custom;
        }
    }

    if (mode & ModeExplode) {
        if (ImGui::CollapsingHeader("Per-shard explode",
                                    ImGuiTreeNodeFlags_DefaultOpen)) {
            bool ed = false;
            ed |= ImGui::SliderFloat("Shard density (c/m)##fsd", &shardDensity, 0.3f, 5.0f);
            ed |= ImGui::SliderFloat("Direction jitter##fsj",     &shardJitter,  0.0f, 1.5f);
            ed |= ImGui::SliderFloat("Spin##fss",                  &shardSpin,    0.0f, 1.0f);
            ed |= ImGui::SliderFloat("Strength (m)##fst",          &explodeStrength, 0.0f, 6.0f);
            if (ed) currentPreset = Preset::Custom;
            ImGui::TextDisabled("Renderer caps outward push at 0.5 m.");
        }
    }

    if (mode & ModeGlitch) {
        if (ImGui::CollapsingHeader("Glitch tear",
                                    ImGuiTreeNodeFlags_DefaultOpen)) {
            bool ed = false;
            ed |= ImGui::SliderFloat("Frequency (bands/m)##fgf", &glitchFrequency, 0.5f, 30.0f);
            ed |= ImGui::SliderFloat("Magnitude (m)##fgm",        &glitchMagnitude, 0.0f, 2.0f);
            ed |= ImGui::SliderFloat("Speed (Hz)##fgs",           &glitchSpeed,     0.1f, 30.0f);
            ed |= ImGui::SliderFloat("Dropout##fgd",              &glitchDropout,   0.0f, 0.8f);
            if (ed) currentPreset = Preset::Custom;
        }
    }

    if (mode == 0) {
        ImGui::TextDisabled("Pick a mode above to enable the fracture pass.");
    }
}

} // namespace spacegen
