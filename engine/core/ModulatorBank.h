#pragma once
// ModulatorBank — global pool of N named LFOs that any layer parameter can
// subscribe to. Operator configures them once in the "Modulators" panel,
// then on each layer parameter picks "Source: LFO 3" + a depth multiplier.
//
// This is the pragmatic v1 of the parameter-graph-spacegen skill: full
// Parameter+Registry comes later. For now, each layer holds integer slot
// references (0 = unbound, 1..N = bank slot) per modulatable field.

#include "LFO.h"

#include <array>
#include <string>

namespace spacegen {

constexpr int kModulatorBankSize = 8;

struct ModulatorEntry {
    std::string name;
    LFO         lfo;
};

class ModulatorBank {
public:
    ModulatorBank() {
        for (int i = 0; i < kModulatorBankSize; ++i) {
            entries_[i].name = "LFO " + std::to_string(i + 1);
            entries_[i].lfo.enabled  = false;
            entries_[i].lfo.amplitude = 1.0f;
            entries_[i].lfo.freqHz    = 0.5f;
            entries_[i].lfo.phase     = 0.0f;
        }
    }

    ModulatorEntry&       entry(int idx)       { return entries_[clamp(idx)]; }
    const ModulatorEntry& entry(int idx) const { return entries_[clamp(idx)]; }

    // Evaluate slot 1..N at time t. Slot 0 (unbound) returns 0.
    // Returns the LFO output normalized to amplitude=1; the subscriber
    // multiplies by its own depth value. `extraPhase` lets the subscriber
    // shift the cycle per fixture for chase effects.
    float eval(int slot1Based, double t, float extraPhase = 0.0f) const {
        if (slot1Based < 1 || slot1Based > kModulatorBankSize) return 0.0f;
        const auto& e = entries_[slot1Based - 1];
        if (!e.lfo.enabled) return 0.0f;
        LFO clone = e.lfo;
        clone.amplitude = 1.0f;
        return clone.eval(t, extraPhase);
    }

private:
    std::array<ModulatorEntry, kModulatorBankSize> entries_;

    static int clamp(int i) {
        if (i < 0) return 0;
        if (i >= kModulatorBankSize) return kModulatorBankSize - 1;
        return i;
    }
};

} // namespace spacegen
