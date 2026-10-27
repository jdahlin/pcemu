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
struct NativeCodeTail {
    uint32_t ip=0,page=0,generation=0;
    bool present=false;
    bool earlyFetchSafe=false, requiresWarmRead=false, requiresUsableSegments=false;
};
struct NativeFarTransfer {
    NativeMemoryMove memory;
    unsigned width=2;
    uint32_t offset=0,end=0;
    uint16_t selector=0;
    bool call=false,indirect=false;
};
struct NativeFarReturn {
    unsigned width=2;
    uint16_t extra=0;
    bool iret=false;
};
// Deferred scalar I/O is executed after returning from the generated frame.
// Callbacks and permission faults must never unwind across generated ARM64.
struct NativePortIO {
    unsigned width=0;
    uint16_t port=0;
    bool dx=false,output=false;
    uint32_t start=0,end=0;
};
inline constexpr uint32_t nativePortIOExit=0x80000000u;
inline constexpr uint32_t nativeSegmentExit=0x04000000u;
inline constexpr uint32_t nativeArithmeticExit=0x02000000u;
inline constexpr uint32_t nativeStringExit=0x10000000u;
inline constexpr uint32_t nativeInterruptExit=0x08000000u;
inline constexpr uint32_t nativeX87Exit=0x20000000u;
inline constexpr uint32_t nativeInvalidOpcodeExit=0x40000000u;
// A decoded, terminal string batch. Host helpers may execute only after a
// side-effect-free preflight validates every required RAM span.
using NativeString = CPU::DecodedString;
} // namespace pc
