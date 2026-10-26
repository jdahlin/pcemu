#pragma once
#include "cpu.hpp"
namespace pc {
// Decoded guest memory access shared by the IR and guarded runtime helper.
struct NativeMemoryMove {
    int base = -1, index = -1, scale = 0, segment = DS, reg = 0, width = 4;
    uint32_t displacement = 0, begin = 0, end = 0;
    bool address32 = false, store = false;
    int delta = 0; // Nonzero for guarded memory INC/DEC; return original value for flags.
    int alu = -1; // Arithmetic/logical RMW; preserve old value for deferred flags.
};
} // namespace pc
