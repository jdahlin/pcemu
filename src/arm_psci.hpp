#pragma once
#include <cstdint>
namespace pc {
// Power State Coordination Interface, ARM DEN 0022C (PSCI 1.0/0.2 errata).
// https://documentation-service.arm.com/static/5f905b78f86e16515cdc1fca
// Stateless PSCI 0.2 fallback; the platform owns live CPU state and applies actions.
enum class ArmPowerAction { None, Off, Reset };
struct ArmPsciResult {
    // Firmware result value, including signed errors or a successful query value.
    int64_t value;
    // Host-side VM lifecycle request; not a value returned in a guest register.
    ArmPowerAction action = ArmPowerAction::None;
};
// Dispatch a function ID and its first two arguments against one online boot CPU.
ArmPsciResult call_psci(uint32_t function_id, uint64_t argument0, uint64_t argument1);
} // namespace pc
