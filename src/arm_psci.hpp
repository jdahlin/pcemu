#pragma once
#include <cstdint>
namespace pc {
enum class ArmPowerAction { None, Off, Reset };
struct ArmPsciResult { int64_t value; ArmPowerAction action = ArmPowerAction::None; };
// Stateless PSCI 0.2 fallback. The platform handles CPU_ON/OFF/AFFINITY_INFO
// against its live CPU state; this fallback models one always-online boot CPU.
ArmPsciResult armPsciCall(uint32_t function, uint64_t arg0, uint64_t arg1);
} // namespace pc
