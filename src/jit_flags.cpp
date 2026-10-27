#include "jit_flags.hpp"
namespace pc::jit_detail {
std::vector<bool> analyzeFlagLiveness(const std::vector<Instruction> &ir) {
    constexpr unsigned arithmeticFlags = CF | PF | AF | ZF | SF | OF;
    std::vector<bool> neededFlags(ir.size());
    unsigned liveFlags = arithmeticFlags; // Every block exit observes full flags.
    for (size_t n = ir.size(); n-- > 0;) {
        const auto &i = ir[n];
        if (i.kind == Kind::Branch && i.dst >= 0) {
            // Either outcome can expose this exact flags image through a trace
            // side exit, even if the predicted continuation overwrites flags.
            liveFlags = arithmeticFlags;
            continue;
        }
        if (i.kind == Kind::Memory) {
            // A memory operation may replay before access or exit after a
            // successful access changes code. Keep both restart states exact,
            // including comparison flags even if a later ALU overwrites them.
            neededFlags[n] = i.memoryAlu || i.memoryMultiply || i.memoryShift;
            liveFlags = arithmeticFlags;
            continue;
        }
        if (i.kind == Kind::Divide || i.kind == Kind::SegmentLoad) {
            // Arithmetic/descriptor faults can expose the pre-instruction flags,
            // even when a following ALU would otherwise make them dead.
            liveFlags = arithmeticFlags;
            continue;
        }
        if (i.kind == Kind::LoadFlags) {
            // AH observes the low flags even if a later ALU overwrites them.
            liveFlags |= CF | PF | AF | ZF | SF;
            continue;
        }
        if (i.kind == Kind::Shift && !i.immediate) {
            neededFlags[n] = liveFlags & (CF | PF | ZF | SF | OF);
            // A zero count can preserve every live flag; AF is always preserved.
            continue;
        }
        if (i.kind == Kind::Rotate) {
            // Rotates preserve all flags except CF and, for normalized count
            // one, OF. Carry rotates also consume CF, even with dead outputs.
            // Zero counts preserve the complete input image.
            neededFlags[n] = true;
            liveFlags = arithmeticFlags;
            continue;
        }
        if (i.kind == Kind::DoubleShift) {
            // Runtime-zero counts and preserved flags require exact input state.
            neededFlags[n] = true;
            liveFlags = arithmeticFlags;
            continue;
        }
        unsigned written = 0;
        if (i.kind == Kind::Alu || i.kind == Kind::Test)
            written = i.kind == Kind::Alu && i.op >= 8 ? arithmeticFlags & ~CF : arithmeticFlags;
        else if (i.kind == Kind::BitTest)
            written = CF;
        else if (i.kind == Kind::Multiply || i.kind == Kind::MultiplyByte)
            written = CF | OF;
        else if (i.kind == Kind::Shift)
            written = CF | SF | ZF | PF | (i.imm == 1 ? OF : 0);
        neededFlags[n] = written & liveFlags;
        liveFlags &= ~written;
        if (i.kind == Kind::Alu && (i.op == 2 || i.op == 3))
            liveFlags |= CF; // Carry is an input even when output flags are dead.
    }
    return neededFlags;
}
} // namespace pc::jit_detail
