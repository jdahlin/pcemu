#pragma once
#include <cstdint>
namespace pc {
// Apply the AArch64 load width, sign extension and destination register size.
uint64_t armMmioLoad(uint64_t value, unsigned width, bool signExtend, bool wideRegister);
} // namespace pc
