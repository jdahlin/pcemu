#pragma once
#include "cpu.hpp"
namespace pc {
// Decoded guest memory access shared by the IR and guarded runtime helper.
struct NativeMemoryMove {
    int base = -1, index = -1, scale = 0, segment = DS, reg = 0, width = 4;
    uint32_t displacement = 0, begin = 0, end = 0;
    bool address32 = false, store = false;
    bool invert = false; // Unary NOT: RMW without flag changes or a register source.
    bool exchange = false; // XCHG returns the original memory value after a successful store.
    int delta = 0; // Nonzero for guarded memory INC/DEC; return original value for flags.
    int bit = -1; // Memory BT/BTS/BTR/BTC selector (4..7); value carries the bit index.
    int alu = -1; // Arithmetic/logical RMW; preserve old value for deferred flags.
};
// A decoded, terminal string batch. Host helpers may execute only after a
// side-effect-free preflight validates every required RAM span.
struct NativeString {
    unsigned opcode = 0xa4, width = 1, repeat = 0, sourceSegment = DS;
    uint32_t start = 0, end = 0;
    bool address32 = false;
};
} // namespace pc
