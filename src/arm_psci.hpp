#pragma once
#include <cstdint>
namespace pc {
enum class ArmPowerAction { None, Off, Reset };
struct ArmPsciResult { int64_t value; ArmPowerAction action = ArmPowerAction::None; };
// Single-CPU PSCI 0.2 subset. No power-down suspend or secondary CPUs.
ArmPsciResult armPsciCall(uint32_t function, uint64_t arg0, uint64_t arg1);
} // namespace pc
