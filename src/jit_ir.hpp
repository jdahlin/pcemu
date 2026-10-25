#pragma once
#include "jit_memory.hpp"
#include <vector>
namespace pc::jit_detail {
// Shared decoded operations; no ARM instruction encoding belongs here.
enum class Kind {
    Unsupported = -1, Move, Alu, Test, Nop, Branch, Memory, Lea,
    CounterBranch, Exchange, Multiply, Shift, IndirectJump, MultiplyByte, DoubleShift
};
struct Instruction {
    Kind kind = Kind::Unsupported;
    int dst, src, op, w, dstShift, srcShift, stackDelta;
    uint32_t imm, end;
    bool immediate;
    bool memoryResult = false; // Read-only memory ALU with a register destination.
    bool memoryAlu = false, reverse = false, nearCall = false, nearReturn = false;
    bool indirectJump = false;
    unsigned sourceWidth = 0;
    bool signedSource = false;
    bool negate = false; // Register NEG uses the ordinary 0 - operand flag path.
    bool predictedTaken = false; // Trace path only; terminal branches use imm/end.
    uint32_t target = 0;
    NativeMemoryMove memory;
};
struct DecodedBlock {
    std::vector<Instruction> instructions;
    uint32_t end = 0;
};
} // namespace pc::jit_detail
