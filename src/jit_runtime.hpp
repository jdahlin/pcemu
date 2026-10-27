#pragma once
#include "jit_memory.hpp"
namespace pc::jit_detail {
// ABI entry points called by generated code. Memory faults are declined and
// replayed by the dispatcher; exceptions must not cross generated frames.
void nativeFinish(CPU *cpu, int op, uint32_t a, uint32_t b, int width, int condition,
                  uint32_t target);
uint32_t nativeSegment(CPU *cpu, unsigned segment, uint32_t selector, unsigned completed) noexcept;
uint32_t nativePopMemory(CPU *cpu, const NativeMemoryMove *destination, uint32_t stackOffset,
                         uint32_t destinationOffset) noexcept;
uint32_t nativeMemory(CPU *cpu, const NativeMemoryMove *op, uint32_t offset, uint32_t value,
                      uint32_t codePage, uint32_t codeGeneration);
} // namespace pc::jit_detail
