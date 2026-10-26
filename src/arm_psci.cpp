// The Power State Coordination Interface (PSCI) lets an ARM guest OS ask firmware
// to idle/start/stop a CPU, query CPU state, or shut down/reset its machine.
// These are firmware calls, not memory-mapped device registers. A function ID
// selects the operation; argument registers carry power states or CPU identities.
// SMC32 calls use 32-bit arguments, while SMC64 calls permit 64-bit arguments.
// The machine handles the SMC/HVC instruction trap and supplies these values.
//
// This stateless fallback models one always-online boot CPU with affinity zero.
// It decodes function IDs, answers discovery/state queries, and returns an action
// for the machine to apply. Off/Reset actions affect the VM, never the host.
// The live platform handles CPU_ON, CPU_OFF and AFFINITY_INFO before reaching
// this fallback, because starting/stopping actual virtual CPUs requires state.
// IMPLEMENTATION NOTE: Standby returns immediately; no host power state changes.
// Power-down/resume and Trusted OS migration are not implemented here.
// Reference: Power State Coordination Interface, ARM DEN 0022C, PSCI 1.0 with
// PSCI 0.2 errata. This implementation advertises the older PSCI 0.2 interface.
// https://documentation-service.arm.com/static/5f905b78f86e16515cdc1fca#page=5

#include "arm_psci.hpp"
namespace pc {
namespace {
// ARM DEN 0022C, 5.1 Function prototypes (function IDs).
// https://documentation-service.arm.com/static/5f905b78f86e16515cdc1fca#page=23
// IDs beginning 0x84 use SMC32; 0xc4 uses SMC64. Bit 30 selects argument width.
// Both encode fast calls in the Standard Secure Service owner range.
enum class Function : uint32_t {
    // Discover the supported PSCI major/minor version.
    PsciVersion = 0x84000000,
    // Idle the calling CPU using a 32-bit power-state argument.
    CpuSuspend32 = 0x84000001,
    // Idle the calling CPU with the 64-bit calling convention.
    CpuSuspend64 = 0xc4000001,
    // Power down the calling CPU; no 64-bit variant is defined.
    CpuOff = 0x84000002,
    // Start a target CPU using 32-bit target/entry-point arguments.
    CpuOn32 = 0x84000003,
    // Start a target CPU using 64-bit target/entry-point arguments.
    CpuOn64 = 0xc4000003,
    // Query a target CPU/topology node using a 32-bit identity.
    AffinityInfo32 = 0x84000004,
    // Query a target CPU/topology node using a 64-bit identity.
    AffinityInfo64 = 0xc4000004,
    // Discover whether a resident Trusted OS requires CPU migration.
    MigrateInfoType = 0x84000006,
    // Request shutdown of the guest-visible system.
    SystemOff = 0x84000008,
    // Request a reset of the guest-visible system.
    SystemReset = 0x84000009,
};
// Function ID bit 30 distinguishes SMC64 from SMC32; it is not an argument bit.
constexpr uint32_t CallingConvention64 = 1u << 30;

// ARM DEN 0022C, 5.2.2 Return error codes, Table 7.
// https://documentation-service.arm.com/static/5f905b78f86e16515cdc1fca#page=35
namespace ReturnCode {
// The requested operation completed successfully.
constexpr int64_t Success = 0;
// This function ID is not implemented by the interface.
constexpr int64_t NotSupported = -1;
// An argument names an unsupported state, CPU or topology level.
constexpr int64_t InvalidParameters = -2;
// The implementation refuses the requested operation.
constexpr int64_t Denied = -3;
// The CPU requested by CPU_ON is already running.
constexpr int64_t AlreadyOn = -4;
} // namespace ReturnCode
// PSCI_VERSION encodes major in bits 31:16 and minor in bits 15:0: version 0.2.
// ARM DEN 0022C, 5.3 PSCI_VERSION.
// https://documentation-service.arm.com/static/5f905b78f86e16515cdc1fca#page=36
constexpr int64_t SupportedVersion = (0u << 16) | 2u;
// ARM DEN 0022C, 5.6 CPU_ON and 5.7 AFFINITY_INFO.
// https://documentation-service.arm.com/static/5f905b78f86e16515cdc1fca#page=43
// https://documentation-service.arm.com/static/5f905b78f86e16515cdc1fca#page=44
// The fallback's sole boot CPU has affinity identity zero (CPU_ON/AFFINITY_INFO).
constexpr uint64_t BootCpuAffinity = 0;
// AFFINITY_INFO level zero addresses an individual CPU, not a cluster.
constexpr uint64_t CpuAffinityLevel = 0;
// AFFINITY_INFO reports ON as zero; this is a state value, not an error code.
constexpr int64_t AffinityStateOn = 0;
// MIGRATE_INFO_TYPE value 2 means no Trusted OS, or one needing no migration.
// A Trusted OS is secure-world software that might otherwise be tied to one CPU.
// ARM DEN 0022C, 5.1.7 MIGRATE_INFO_TYPE.
// https://documentation-service.arm.com/static/5f905b78f86e16515cdc1fca#page=28
constexpr int64_t TrustedOsMigrationNotRequired = 2;
// ARM DEN 0022C, 5.4 CPU_SUSPEND.
// https://documentation-service.arm.com/static/5f905b78f86e16515cdc1fca#page=37
// CPU_SUSPEND original format: state ID 0, standby type, affinity level 0.
constexpr uint64_t CpuStandbyPowerState = 0;

// Answer CPU_SUSPEND for the one supported standby state without changing CPU state.
// ARM DEN 0022C, 5.4 CPU_SUSPEND (power_state encoding and standby behavior).
// https://documentation-service.arm.com/static/5f905b78f86e16515cdc1fca#page=37
ArmPsciResult cpu_suspend(uint64_t power_state) {
    // IMPLEMENTATION NOTE: Standby can wake immediately; no wait is imposed here.
    // FIXME: Implement power-down/resume before accepting deeper power states.
    return {power_state == CpuStandbyPowerState ? ReturnCode::Success
                                                : ReturnCode::InvalidParameters};
}

// Answer CPU_ON against the fallback's already-running boot CPU.
// ARM DEN 0022C, 5.6 CPU_ON.
// https://documentation-service.arm.com/static/5f905b78f86e16515cdc1fca#page=43
ArmPsciResult cpu_on(uint64_t target_cpu) {
    return {target_cpu == BootCpuAffinity ? ReturnCode::AlreadyOn : ReturnCode::InvalidParameters};
}

// Report the boot CPU as ON, rejecting other identities and topology levels.
// ARM DEN 0022C, 5.7 AFFINITY_INFO.
// https://documentation-service.arm.com/static/5f905b78f86e16515cdc1fca#page=44
ArmPsciResult affinity_info(uint64_t target_affinity, uint64_t lowest_affinity_level) {
    return {target_affinity == BootCpuAffinity && lowest_affinity_level == CpuAffinityLevel
                ? AffinityStateOn
                : ReturnCode::InvalidParameters};
}
} // namespace

// Decode a PSCI call, returning its value and any machine lifecycle action.
// ARM DEN 0022C, 5.1 Function prototypes and 5.2.1 Register usage.
// https://documentation-service.arm.com/static/5f905b78f86e16515cdc1fca#page=23
// https://documentation-service.arm.com/static/5f905b78f86e16515cdc1fca#page=35
ArmPsciResult call_psci(uint32_t function_id, uint64_t argument0, uint64_t argument1) {
    // COMPATIBILITY: SMC32 calls discard argument high words, including on AArch64.
    if (!(function_id & CallingConvention64)) {
        argument0 = uint32_t(argument0);
        argument1 = uint32_t(argument1);
    }
    switch (static_cast<Function>(function_id)) {
    case Function::PsciVersion:
        return {SupportedVersion};
    case Function::CpuSuspend32:
    case Function::CpuSuspend64:
        return cpu_suspend(argument0);
    case Function::CpuOff:
        // COMPATIBILITY: The stateless fallback refuses to remove its only CPU.
        // Live CPU_OFF handling belongs to the machine; this path has no CPU state.
        // ARM DEN 0022C, 5.5 CPU_OFF.
        // https://documentation-service.arm.com/static/5f905b78f86e16515cdc1fca#page=41
        return {ReturnCode::Denied};
    case Function::SystemOff:
        // IMPLEMENTATION NOTE: Return a lifecycle action; the machine must prevent
        // further guest execution instead of returning from successful SYSTEM_OFF.
        // ARM DEN 0022C, 5.10 SYSTEM_OFF.
        // https://documentation-service.arm.com/static/5f905b78f86e16515cdc1fca#page=47
        return {ReturnCode::Success, ArmPowerAction::Off};
    case Function::SystemReset:
        // As for SYSTEM_OFF, the machine applies the non-returning reset action.
        // ARM DEN 0022C, 5.11 SYSTEM_RESET.
        // https://documentation-service.arm.com/static/5f905b78f86e16515cdc1fca#page=48
        return {ReturnCode::Success, ArmPowerAction::Reset};
    case Function::CpuOn32:
    case Function::CpuOn64:
        return cpu_on(argument0);
    case Function::AffinityInfo32:
    case Function::AffinityInfo64:
        return affinity_info(argument0, argument1);
    case Function::MigrateInfoType:
        return {TrustedOsMigrationNotRequired};
    default:
        return {ReturnCode::NotSupported};
    }
}
} // namespace pc
