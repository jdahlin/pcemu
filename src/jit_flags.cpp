#include "jit_flags.hpp"
namespace pc::jit_detail {
std::vector<bool> analyzeFlagLiveness(const std::vector<Instruction> &ir) {
    constexpr unsigned arithmeticFlags = CF | PF | AF | ZF | SF | OF;
    std::vector<bool> neededFlags(ir.size());
    unsigned liveFlags = arithmeticFlags; // Every block exit observes full flags.
    for (size_t n = ir.size(); n-- > 0;) {
        const auto &i = ir[n];
        if (i.kind == Kind::Memory) {
            // A memory operation may replay before access or exit after a
            // successful access changes code. Keep both restart states exact,
            // including comparison flags even if a later ALU overwrites them.
            neededFlags[n] = i.compareMemory;
            liveFlags = arithmeticFlags;
            continue;
        }
        unsigned written = 0;
        if (i.kind == Kind::Alu || i.kind == Kind::Test)
            written = i.kind == Kind::Alu && i.op >= 8 ? arithmeticFlags & ~CF : arithmeticFlags;
        else if (i.kind == Kind::Multiply)
            written = CF | OF;
        else if (i.kind == Kind::Shift)
            written = CF | SF | ZF | PF | (i.imm == 1 ? OF : 0);
        neededFlags[n] = written & liveFlags;
        liveFlags &= ~written;
    }
    return neededFlags;
}
} // namespace pc::jit_detail
