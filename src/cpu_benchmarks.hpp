#pragma once
#include <cstdint>
namespace pc {
double benchmarkCpuRegisters(bool jit, uint64_t target);
double benchmarkCpuMemory(bool jit, uint64_t target, bool paging);
} // namespace pc
