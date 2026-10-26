// The ARM OS Lock protects debug register state while an operating system saves
// or restores it. This per-CPU model supplies the lock access/status registers
// when Hypervisor.framework traps guest system-register instructions. Firmware
// or the OS writes OSLAR_EL1 to change the lock and reads OSLSR_EL1 to inspect it.
// OSDLR_EL1 adds a double lock which this model retains until CPU state is reset.
// The machine decodes the operand register and advances the guest instruction;
// this component only consumes the exception syndrome and reads/writes its value.
// Breakpoint/watchpoint execution remains owned by Hypervisor.framework.
//
// IMPLEMENTATION NOTE: A newly constructed CPU has the OS lock set (cold-reset
// state) and double lock clear. The model advertises OS Lock model 0b10.
// COMPATIBILITY: Double lock is modeled as a sticky software latch. Debug power
// requests, debug state and feature-dependent register availability are not modeled.
// FIXME: Model FEAT_DoubleLock and the CORENPDRQ/Non-debug conditions before
// claiming full double-lock conformance; retain current guest-visible behavior.
// Arm AArch64 System Registers, OSLAR_EL1 / OSLSR_EL1 / OSDLR_EL1
// (Arm register descriptions mirrored by Lund University).
// https://df.lth.se/~getz/ARM/SysReg/AArch64-oslar_el1.html
// https://df.lth.se/~getz/ARM/SysReg/AArch64-oslsr_el1.html
// https://df.lth.se/~getz/ARM/SysReg/AArch64-osdlr_el1.html

#include "arm_os_lock.hpp"
namespace pc {
namespace {
// ESR_EL2 exception class occupies bits [31:26]. Class 0x18 identifies a
// trapped AArch64 MRS/MSR system-register access (among other system operations).
// https://df.lth.se/~getz/ARM/SysReg/AArch64-esr_el2.html
constexpr unsigned ExceptionClassShift = 26;
constexpr uint64_t ExceptionClassMask = 0x3f;
constexpr uint64_t SystemRegisterExceptionClass = 0x18;
// ESR_EL2.IL, bit 25, must report a 32-bit instruction for this trap class.
constexpr uint64_t InstructionLength32 = 1ull << 25;
// ESR_EL2.ISS system-register fields: op0[21:20], op2[19:17], op1[16:14],
// CRn[13:10], CRm[4:1], direction[0] (read=1). Exclude Rt[9:5]: the
// machine supplies the operand value regardless of which CPU register is used.
constexpr uint64_t SystemRegisterEncodingMask = 0x3ffc1f;
// Encodings below include the ISS direction bit, so unsupported access directions
// do not accidentally match a write-only/read-only register.
enum SystemRegisterAccess : uint64_t {
    // MRS OSDLR_EL1, S2_0_C1_C3_4: read the double-lock control bit.
    // https://df.lth.se/~getz/ARM/SysReg/AArch64-osdlr_el1.html
    ReadOsDoubleLockRegister = 0x280407,
    // MSR OSDLR_EL1, S2_0_C1_C3_4: write the double-lock control bit.
    WriteOsDoubleLockRegister = 0x280406,
    // MRS OSLSR_EL1, S2_0_C1_C1_4: read lock model and current lock state.
    // https://df.lth.se/~getz/ARM/SysReg/AArch64-oslsr_el1.html
    ReadOsLockStatusRegister = 0x280403,
    // MSR OSLAR_EL1, S2_0_C1_C0_4: copy the operand's OSLK bit to the lock.
    // https://df.lth.se/~getz/ARM/SysReg/AArch64-oslar_el1.html
    WriteOsLockAccessRegister = 0x280400,
};
// OSLAR_EL1.OSLK and OSDLR_EL1.DLK use bit 0 of their respective write values.
constexpr uint64_t LockControl = 1;
// OSLSR_EL1.OSLM is split across bits [3,0]; 0b10 means OS Lock implemented.
// nTT (bit 2) and reserved bits read as zero.
constexpr uint64_t OsLockModelImplemented = 1ull << 3;
// OSLSR_EL1.OSLK (bit 1) reports whether the OS lock is currently set.
constexpr uint64_t OsLockStatus = 1ull << 1;
} // namespace

// Handle supported lock-register MRS/MSR traps. Reads replace value; writes use
// its low control bit. Return false, leaving value/state intact, for other traps.
// Arm ESR_EL2 system-register ISS, OSLAR_EL1, OSLSR_EL1 and OSDLR_EL1 accesses.
// https://df.lth.se/~getz/ARM/SysReg/AArch64-esr_el2.html
// https://df.lth.se/~getz/ARM/SysReg/AArch64-oslar_el1.html
// https://df.lth.se/~getz/ARM/SysReg/AArch64-oslsr_el1.html
// https://df.lth.se/~getz/ARM/SysReg/AArch64-osdlr_el1.html
bool ArmOsLock::access_system_register(uint64_t syndrome, uint64_t &value) {
    if ((syndrome >> ExceptionClassShift & ExceptionClassMask) != SystemRegisterExceptionClass ||
        !(syndrome & InstructionLength32))
        return false;
    const auto encoding = syndrome & SystemRegisterEncodingMask;
    if (encoding == SystemRegisterAccess::ReadOsDoubleLockRegister) {
        value = doubleLocked;
        return true;
    }
    if (encoding == SystemRegisterAccess::WriteOsDoubleLockRegister) {
        // COMPATIBILITY: Once set, the software double lock clears only on CPU reset.
        doubleLocked |= value & LockControl;
        return true;
    }
    if (encoding == SystemRegisterAccess::ReadOsLockStatusRegister) {
        value = OsLockModelImplemented | (locked ? OsLockStatus : 0);
        return true;
    }
    if (encoding == SystemRegisterAccess::WriteOsLockAccessRegister) {
        if (!doubleLocked)
            locked = value & LockControl;
        return true;
    }
    return false;
}
} // namespace pc
