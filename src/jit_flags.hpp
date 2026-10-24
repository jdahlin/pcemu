#pragma once
#include "jit_ir.hpp"
namespace pc::jit_detail {
// Flags required by block exits and precise memory restart/invalidation exits.
std::vector<bool> analyzeFlagLiveness(const std::vector<Instruction> &ir);
} // namespace pc::jit_detail
