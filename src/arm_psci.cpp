#include "arm_psci.hpp"
namespace pc {
ArmPsciResult armPsciCall(uint32_t function, uint64_t arg0, uint64_t arg1) {
    if (!(function & 0x40000000)) { arg0 = uint32_t(arg0); arg1 = uint32_t(arg1); }
    switch (function) {
    case 0x84000000: return {2}; // PSCI 0.2
    case 0x84000001: case 0xc4000001:
        // Standby can wake spuriously; deeper states have not been implemented.
        return {arg0 == 0 ? 0 : -2};
    case 0x84000002: return {-3}; // Stateless single-CPU fallback denies the final CPU_OFF.
    case 0x84000008: return {0, ArmPowerAction::Off};
    case 0x84000009: return {0, ArmPowerAction::Reset};
    case 0x84000003: case 0xc4000003: return {arg0 == 0 ? -4 : -2};
    case 0x84000004: case 0xc4000004: return {arg0 == 0 && arg1 == 0 ? 0 : -2};
    case 0x84000006: return {2}; // No resident trusted OS.
    default: return {-1};
    }
}
} // namespace pc
