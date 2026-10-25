#include "jit.hpp"
#include "jit_ir.hpp"
#include <utility>
namespace pc {
using jit_detail::Kind;
jit_detail::DecodedBlock Jit::decode(Block &block) {
    using IR = jit_detail::Instruction;
    auto linear = cpu.address(CS, cpu.ip);
    std::vector<IR> ir;
    uint32_t pos = cpu.ip;
    auto ipmask = cpu.s[CS].big ? 0xffffffffu : 65535u;
    auto byte = [&]() {
        if ((cpu.address(CS, pos) >> 12) != (linear >> 12))
            throw 0;
        auto p = cpu.physical(cpu.address(CS, pos));
        if ((p >> 12) != block.page)
            throw 0;
        cpu.markCode(p);
        auto b = cpu.mem.at(p);
        pos = (pos + 1) & ipmask;
        return b;
    };
    auto immediate = [&](int n) {
        uint32_t v = 0;
        for (int i = 0; i < n; i++)
            v |= uint32_t(byte()) << (i * 8);
        return v;
    };
    auto memoryOperand = [&](uint8_t m, bool address32, int overrideSegment) {
        NativeMemoryMove move;
        move.address32 = address32;
        unsigned mod = m >> 6, rm = m & 7;
        if (address32) {
            if (rm == 4) {
                auto sib = byte();
                move.scale = sib >> 6;
                move.index = ((sib >> 3) & 7) == 4 ? -1 : (sib >> 3) & 7;
                rm = sib & 7;
            }
            if (mod == 0 && rm == 5)
                move.displacement = immediate(4);
            else {
                move.base = rm;
                if (rm == BP || rm == SP)
                    move.segment = SS;
            }
            if (mod == 1)
                move.displacement += uint32_t(int32_t(int8_t(byte())));
            else if (mod == 2)
                move.displacement += immediate(4);
        } else {
            static const int bases[] = {BX, BX, BP, BP, SI, DI, BP, BX};
            static const int indices[] = {SI, DI, SI, DI, -1, -1, -1, -1};
            if (mod == 0 && rm == 6)
                move.displacement = immediate(2);
            else {
                move.base = bases[rm];
                move.index = indices[rm];
                if (move.base == BP)
                    move.segment = SS;
            }
            if (mod == 1)
                move.displacement += uint32_t(int32_t(int8_t(byte())));
            else if (mod == 2)
                move.displacement += immediate(2);
        }
        if (overrideSegment >= 0)
            move.segment = overrideSegment;
        return move;
    };
    for (unsigned count = 0; count < 64; count++) {
        auto begin = pos;
        auto savedCR2 = cpu.cr[2];
        try {
            int w = cpu.s[CS].big ? 4 : 2;
            bool address32 = cpu.s[CS].big;
            int overrideSegment = -1;
            auto op = byte();
            for (unsigned prefixes = 0;; prefixes++) {
                if (prefixes >= 15)
                    throw 0;
                if (op == 0x66)
                    w = cpu.s[CS].big ? 2 : 4;
                else if (op == 0x67)
                    address32 = !cpu.s[CS].big;
                else if (op == 0x26 || op == 0x2e || op == 0x36 || op == 0x3e)
                    overrideSegment = (op >> 3) & 3;
                else if (op == 0x64 || op == 0x65)
                    overrideSegment = op == 0x64 ? FS : GS;
                else
                    break;
                op = byte();
            }
            IR i{};
            i.w = w;
            i.kind = Kind::Unsupported;
            i.dst = 0;
            i.src = 0;
            if (op == 0xc2 || op == 0xc3) {
                i.kind = Kind::Memory;
                i.nearReturn = true;
                i.stackDelta = w + (op == 0xc2 ? immediate(2) : 0);
                i.memory.base = SP;
                i.memory.segment = SS;
                i.memory.address32 = block.stack32;
                block.usesStack = true;
            } else if (op == 0xe8) {
                // A direct near CALL is a restartable push followed by a jump.
                // Keep the return address separate from the committed target,
                // including when the push invalidates this block's code page.
                auto displacement = immediate(w);
                i.kind = Kind::Memory;
                i.nearCall = i.immediate = i.memory.store = true;
                i.imm = pos;
                i.target = (pos + (w == 2 ? uint32_t(int32_t(int16_t(displacement)))
                                         : displacement)) & (w == 2 ? 65535u : UINT32_MAX);
                i.stackDelta = -w;
                i.memory.base = SP;
                i.memory.segment = SS;
                i.memory.address32 = block.stack32;
                i.memory.displacement = uint32_t(-w);
                block.usesStack = true;
            } else if ((op >= 0x50 && op <= 0x5f) || op == 0x68 || op == 0x6a) {
                i.kind = Kind::Memory;
                i.immediate = op == 0x68 || op == 0x6a;
                i.dst = i.memory.reg = i.immediate ? 0 : op & 7;
                i.memory.store = i.immediate || op < 0x58;
                if (i.immediate)
                    i.imm = op == 0x6a ? uint32_t(int32_t(int8_t(byte()))) : immediate(w);
                i.stackDelta = i.memory.store ? -w : w;
                i.memory.base = SP;
                i.memory.segment = SS;
                i.memory.address32 = block.stack32;
                i.memory.displacement = i.memory.store ? uint32_t(-w) : 0;
                block.usesStack = true;
            } else if ((op >= 0x40 && op <= 0x4f) || op == 0xfe || op == 0xff) {
                i.kind = Kind::Alu;
                i.immediate = true;
                i.imm = 1;
                if (op == 0xfe || op == 0xff) {
                    auto m = byte();
                    if (op == 0xff && ((m >> 3) & 7) == 4) {
                        i.indirectJump = true;
                        i.imm = 0;
                        if ((m >> 6) == 3) {
                            i.kind = Kind::IndirectJump;
                            i.immediate = false;
                            i.dst = i.src = m & 7;
                        } else {
                            i.kind = Kind::Memory;
                            i.memory = memoryOperand(m, address32, overrideSegment);
                        }
                    } else if (((m >> 3) & 7) > 1) {
                        pos = begin;
                        break;
                    }
                    if (!i.indirectJump) {
                        i.w = op == 0xfe ? 1 : w;
                        i.op = 8 + ((m >> 3) & 1);
                        if ((m >> 6) != 3) {
                            i.kind = Kind::Memory;
                            i.memoryAlu = true;
                            i.memory = memoryOperand(m, address32, overrideSegment);
                            i.memory.store = true;
                            i.memory.delta = i.op == 8 ? 1 : -1;
                        } else
                            i.dst = m & 7;
                    }
                } else {
                    i.dst = op & 7;
                    i.op = op < 0x48 ? 8 : 9;
                }
            } else if (op == 0xc0 || op == 0xc1 || op == 0xd0 || op == 0xd1) {
                auto m = byte();
                if ((m >> 6) != 3 || ((m >> 3) & 7) < 4) {
                    pos = begin;
                    break;
                }
                i.kind = Kind::Shift;
                i.w = (op & 1) ? w : 1;
                i.dst = m & 7;
                i.op = (m >> 3) & 7;
                i.immediate = true;
                i.imm = (op < 0xd0 ? byte() : 1) & 31;
                if (!i.imm)
                    i.kind = Kind::Nop; // A masked zero count preserves all flags/GPRs.
            } else if (op == 0x69 || op == 0x6b) {
                auto m = byte();
                if ((m >> 6) != 3) {
                    pos = begin;
                    break;
                }
                i.kind = Kind::Multiply; // Three-operand signed register multiply.
                i.dst = (m >> 3) & 7;
                i.src = m & 7;
                i.immediate = true;
                i.imm = op == 0x6b ? uint32_t(int32_t(int8_t(byte()))) : immediate(w);
            } else if (op == 0x9f) {
                i.kind = Kind::LoadFlags;
                i.dst = AX;
                i.immediate = true;
            } else if (op >= 0xb0 && op <= 0xbf) {
                i.kind = Kind::Move;
                i.w = op < 0xb8 ? 1 : w;
                i.dst = op & 7;
                i.immediate = true;
                i.imm = immediate(i.w);
            } else if (op == 0x86 || op == 0x87) {
                auto m = byte();
                if ((m >> 6) != 3) {
                    pos = begin;
                    break;
                }
                i.kind = Kind::Exchange;
                i.w = op == 0x86 ? 1 : w;
                i.dst = m & 7;
                i.src = (m >> 3) & 7;
            } else if (op >= 0x91 && op <= 0x97) {
                i.kind = Kind::Exchange;
                i.dst = AX;
                i.src = op & 7;
            } else if (op >= 0x88 && op <= 0x8b) {
                i.w = (op & 1) ? w : 1;
                auto m = byte();
                if ((m >> 6) != 3) {
                    i.kind = Kind::Memory;
                    i.memory = memoryOperand(m, address32, overrideSegment);
                    i.dst = i.memory.reg = (m >> 3) & 7;
                    i.memory.store = !(op & 2);
                } else {
                    i.kind = Kind::Move;
                    i.dst = !(op & 2) ? m & 7 : (m >> 3) & 7;
                    i.src = !(op & 2) ? (m >> 3) & 7 : m & 7;
                }
            } else if (op == 0x8d) {
                auto m = byte();
                if ((m >> 6) == 3) {
                    pos = begin;
                    break;
                }
                i.kind = Kind::Lea; // LEA computes an offset without accessing the segment or RAM.
                i.dst = (m >> 3) & 7;
                i.memory = memoryOperand(m, address32, overrideSegment);
            } else if (op >= 0xa0 && op <= 0xa3) {
                i.kind = Kind::Memory;
                i.w = (op & 1) ? w : 1;
                i.dst = i.memory.reg = AX;
                i.memory.segment = overrideSegment < 0 ? DS : overrideSegment;
                i.memory.address32 = address32;
                i.memory.displacement = immediate(address32 ? 4 : 2);
                i.memory.store = bool(op & 2);
            } else if (op < 0x40 && (op & 7) <= 5) {
                i.kind = Kind::Alu;
                i.w = (op & 1) ? w : 1;
                i.op = op >> 3;
                if ((op & 7) >= 4) {
                    i.dst = AX;
                    i.immediate = true;
                    i.imm = immediate(i.w);
                } else {
                    auto m = byte();
                    if ((m >> 6) != 3) {
                        if (i.op == 2 || i.op == 3 || (i.op != 7 && !(op & 2))) {
                            pos = begin;
                            break;
                        }
                        i.kind = Kind::Memory;
                        i.memoryAlu = true;
                        i.memoryResult = i.op != 7;
                        i.reverse = op & 2;
                        i.memory = memoryOperand(m, address32, overrideSegment);
                        i.dst = (m >> 3) & 7;
                    } else {
                        i.dst = (op & 2) ? (m >> 3) & 7 : m & 7;
                        i.src = (op & 2) ? m & 7 : (m >> 3) & 7;
                    }
                }
            } else if (op >= 0x80 && op <= 0x83) {
                i.w = (op == 0x80 || op == 0x82) ? 1 : w;
                auto m = byte();
                i.op = (m >> 3) & 7;
                if ((m >> 6) != 3) {
                    if (i.op != 7) {
                        pos = begin;
                        break;
                    }
                    i.memoryAlu = true;
                    i.memory = memoryOperand(m, address32, overrideSegment);
                }
                i.kind = i.memoryAlu ? Kind::Memory : Kind::Alu;
                i.dst = i.memoryAlu ? 0 : m & 7;
                i.immediate = true;
                i.imm = op == 0x83 ? uint32_t(int32_t(int8_t(byte()))) : immediate(i.w);
            } else if (op == 0x84 || op == 0x85) {
                i.w = (op & 1) ? w : 1;
                auto m = byte();
                i.op = 4;
                if ((m >> 6) != 3) {
                    i.kind = Kind::Memory;
                    i.memoryAlu = true;
                    i.memory = memoryOperand(m, address32, overrideSegment);
                    i.dst = (m >> 3) & 7;
                } else {
                    i.kind = Kind::Test;
                    i.dst = m & 7;
                    i.src = (m >> 3) & 7;
                }
            } else if (op == 0xa8 || op == 0xa9) {
                i.kind = Kind::Test;
                i.w = op == 0xa8 ? 1 : w;
                i.op = 4;
                i.dst = AX;
                i.immediate = true;
                i.imm = immediate(i.w);
            } else if (op == 0xf6 || op == 0xf7) {
                i.w = op == 0xf6 ? 1 : w;
                auto m = byte();
                if (op == 0xf6 && (m & 0x38) == 0x20 && (m >> 6) == 3) {
                    i.kind = Kind::MultiplyByte;
                    i.dst = AX;
                    i.src = m & 3;
                    i.srcShift = (m & 4) ? 8 : 0;
                } else if ((m & 0x38) == 0x18 && (m >> 6) == 3) {
                    i.kind = Kind::Alu;
                    i.op = 5;
                    i.negate = true;
                    i.dst = i.src = m & 7;
                } else {
                    if (m & 0x38) {
                        pos = begin;
                        break;
                    }
                    i.op = 4;
                    i.immediate = true;
                    if ((m >> 6) == 3) {
                        i.kind = Kind::Test;
                        i.dst = m & 7;
                    } else {
                        i.kind = Kind::Memory;
                        i.memoryAlu = true;
                        i.memory = memoryOperand(m, address32, overrideSegment);
                    }
                    i.imm = immediate(i.w);
                }
            } else if (op == 0xe2 || op == 0xe3) {
                auto displacement = int8_t(byte());
                i.kind = Kind::CounterBranch;
                i.dst = CX;
                i.immediate = true;
                i.op = op;
                i.imm = (pos + displacement) & (w == 4 ? UINT32_MAX : 65535u);
                // Address size selects CX/ECX; operand size selects the target
                // IP mask. Neither LOOP nor JCXZ changes guest flags.
                i.w = address32 ? 4 : 2;
            } else if (op == 0x90) {
                i.kind = Kind::Nop;
            } else if (op == 0x0f) {
                auto extended = byte();
                if (extended == 0xba || extended == 0xa3 || extended == 0xab ||
                    extended == 0xb3 || extended == 0xbb) {
                    auto m = byte();
                    // Memory bit strings have different index/address semantics.
                    if ((m >> 6) != 3) { pos = begin; break; }
                    i.kind = Kind::BitTest;
                    i.dst = m & 7;
                    i.src = (m >> 3) & 7;
                    i.immediate = extended == 0xba;
                    i.op = i.immediate ? i.src : extended == 0xa3 ? 4
                           : extended == 0xab ? 5 : extended == 0xb3 ? 6 : 7;
                    if (i.op < 4) { pos = begin; break; }
                    if (i.immediate) i.imm = byte() & (w * 8 - 1);
                } else if (extended == 0xac || extended == 0xad) {
                    auto m = byte();
                    if ((m >> 6) != 3) { pos = begin; break; }
                    i.kind = Kind::DoubleShift;
                    i.dst = m & 7;
                    i.src = (m >> 3) & 7;
                    i.immediate = extended == 0xac;
                    if (i.immediate) {
                        i.imm = byte() & 31;
                        if (!i.imm) i.kind = Kind::Nop;
                    }
                } else if (extended == 0xaf) {
                    auto m = byte();
                    if ((m >> 6) != 3) {
                        pos = begin;
                        break;
                    }
                    i.kind = Kind::Multiply;
                    i.dst = (m >> 3) & 7;
                    i.src = m & 7;
                } else if (extended == 0xb6 || extended == 0xb7 || extended == 0xbe || extended == 0xbf) {
                    auto m = byte();
                    i.kind = (m >> 6) == 3 ? Kind::Move : Kind::Memory;
                    i.dst = (m >> 3) & 7;
                    i.src = m & 7;
                    if (i.kind == Kind::Memory)
                        i.memory = memoryOperand(m, address32, overrideSegment);
                    i.sourceWidth = (extended & 1) ? 2 : 1;
                    i.signedSource = extended & 8;
                    if (i.kind == Kind::Move && i.sourceWidth == 1) {
                        i.srcShift = (i.src & 4) ? 8 : 0;
                        i.src &= 3;
                    }
                } else if (extended >= 0x80 && extended <= 0x8f) {
                    auto raw = immediate(w);
                    auto displacement = w == 2 ? int32_t(int16_t(raw)) : int32_t(raw);
                    i.kind = Kind::Branch;
                    i.dst = extended & 15;
                    i.imm = (pos + displacement) & (w == 4 ? UINT32_MAX : 65535u);
                } else {
                    pos = begin;
                    break;
                }
            } else if (op == 0xeb || op == 0xe9 || (op >= 0x70 && op <= 0x7f)) {
                int displacementWidth = op == 0xe9 ? w : 1;
                i.dst = -1;
                if (op >= 0x70 && op <= 0x7f)
                    i.dst = op & 15;
                auto raw = immediate(displacementWidth);
                int32_t displacement = displacementWidth == 1   ? int8_t(raw)
                                       : displacementWidth == 2 ? int16_t(raw)
                                                                : int32_t(raw);
                i.kind = Kind::Branch;
                i.imm = (pos + displacement) & (w == 4 ? 0xffffffffu : 65535u);
            } else {
                // Unsupported instructions end a block normally. Host stack
                // unwinding here was a measurable whole-application cost.
                pos = begin;
                break;
            }
            i.end = pos;
            if (i.w == 1 && (i.kind <= Kind::Test || i.kind == Kind::Memory || i.kind == Kind::Exchange || i.kind == Kind::Shift)) {
                i.dstShift = (i.dst & 4) ? 8 : 0;
                i.dst &= 3;
                if (!i.immediate && i.kind != Kind::Memory) {
                    i.srcShift = (i.src & 4) ? 8 : 0;
                    i.src &= 3;
                }
            }
            if (i.kind == Kind::Memory) {
                i.memory.width = i.sourceWidth ? i.sourceWidth : i.w;
                i.memory.begin = begin;
                i.memory.end = i.nearCall ? i.target : pos;
            }
            if (i.kind == Kind::Branch && i.dst >= 0)
                i.predictedTaken = i.imm < i.end;
            ir.push_back(i);
            if (i.kind == Kind::Branch && i.dst < 0 && blockMode != BlockMode::Basic) {
                // The next byte fetch enforces the original linear/physical page
                // guard. Failed speculative decoding leaves this jump terminal;
                // its architectural target remains in i.imm.
                pos = i.imm;
                continue;
            }
            if (i.kind == Kind::Branch && blockMode == BlockMode::Trace) {
                pos = i.predictedTaken ? i.imm : i.end;
                continue;
            }
            if (i.kind == Kind::Branch || i.kind == Kind::CounterBranch || i.nearCall || i.nearReturn || i.indirectJump)
                break;
        } catch (int) {
            pos = begin;
            break;
        } catch (const Fault &) {
            // Speculative decoding must not deliver a fault before prior IR runs.
            cpu.cr[2] = savedCR2;
            pos = begin;
            break;
        }
    }
    return {std::move(ir), pos};
}
} // namespace pc
