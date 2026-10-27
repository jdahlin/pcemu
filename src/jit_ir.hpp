#pragma once
#include "jit_memory.hpp"
#include <vector>
namespace pc::jit_detail {
// Shared decoded operations; no ARM instruction encoding belongs here.
enum class Kind {
    Unsupported = -1, Move, Alu, Test, Nop, Branch, Memory, Lea,
    CounterBranch, Exchange, Multiply, Shift, IndirectJump, MultiplyFull, DoubleShift, BitTest,
    LoadFlags, Direction, Rotate, SignExtend, Invert, Divide, SegmentRead, SegmentLoad, InterruptControl, String, StoreFlags, CarryControl, SetCondition, PortIO, FarReturn
};
struct Instruction {
    Kind kind = Kind::Unsupported;
    int dst, src, op, w, dstShift, srcShift, stackDelta;
    uint32_t imm, end;
    uint32_t start = 0; // Guest start, including prefixes and followed jumps.
    bool immediate;
    int setCondition = -1; // SETcc byte result; memory stores save it across access guards.
    int segmentRegister = -1;
    bool segmentLoad = false; // Register/guarded memory selector -> hidden segment state.
    bool xlat = false; // BX/EBX + zero-extended AL, with guarded pre-access replay.
    bool memoryPop = false; // Guard stack read and post-increment destination before access.
    bool memoryDoubleShift = false; // SHLD/SHRD, including address-only zero counts.
    bool memoryShift = false; // RAM-hit shift/rotate RMW; unsafe writes replay before access.
    bool memoryDivide = false; // Native RAM-hit division with guarded interpreter replay.
    bool memoryMultiplyFull = false; // Implicit AX/DX product after a guarded read.
    bool memoryMultiply = false; // Two/three-operand signed multiply after a successful read.
    bool memoryResult = false; // Read-only memory ALU with a register destination.
    bool memoryAlu = false, reverse = false, nearCall = false, nearReturn = false;
    bool pushFlags = false, popFlags = false;
    bool stackMany = false; // PUSHA/POPA: guard the entire RAM span before any access.
    bool leaveFrame = false; // LEAVE reads at BP/EBP and commits SP/BP after success.
    bool stringMove = false; // Non-REP LODS/STOS; index changes only after successful access.
    bool indirectJump = false, indirectCall = false, memoryCall = false, memoryPush = false;
    unsigned sourceWidth = 0;
    bool signedSource = false;
    bool negate = false; // Register NEG uses the ordinary 0 - operand flag path.
    bool predictedTaken = false; // Trace path only; terminal branches use imm/end.
    uint32_t target = 0;
    NativeMemoryMove memory, callPointer;
    NativeString string;
};
struct DecodedBlock {
    std::vector<Instruction> instructions;
    uint32_t end = 0;
    NativePortIO portIO;
};
} // namespace pc::jit_detail
