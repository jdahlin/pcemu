#include "arm_os_lock.hpp"
namespace pc {
bool ArmOsLock::access(uint64_t syndrome, uint64_t &value) {
    // ESR_EL2.ISS: ignore Rt, retain direction and every register encoding bit.
    if ((syndrome >> 26 & 63) != 0x18 || !(syndrome & (1ull << 25))) return false;
    const auto encoding = syndrome & 0x3ffc1f;
    if (encoding == 0x280407) { // MRS OSDLR_EL1: S2_0_C1_C3_4.
        value = doubleLocked;
        return true;
    }
    if (encoding == 0x280406) { // MSR OSDLR_EL1; double lock is sticky until reset.
        doubleLocked |= value & 1;
        return true;
    }
    if (encoding == 0x280403) { // MRS OSLSR_EL1: S2_0_C1_C1_4.
        value = 8 | (locked ? 2 : 0); // OSLM=0b10, nTT=0, OSLK.
        return true;
    }
    if (encoding == 0x280400) { // MSR OSLAR_EL1: S2_0_C1_C0_4.
        if (!doubleLocked) locked = value & 1;
        return true;
    }
    return false;
}
} // namespace pc
