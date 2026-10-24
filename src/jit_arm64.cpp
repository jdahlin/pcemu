#include "jit.hpp"
#include "jit_ir.hpp"
#include "jit_flags.hpp"
#include "jit_runtime.hpp"
#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstring>
#include <type_traits>
#if defined(__APPLE__) && defined(__aarch64__)
#include <libkern/OSCacheControl.h>
#include <pthread.h>
namespace pc {
using jit_detail::Kind;
using jit_detail::nativeFinish;
using jit_detail::nativeMemory;
Jit::Block Jit::emitArm64(Block block, const jit_detail::DecodedBlock &decoded) {
    static_assert(std::is_standard_layout_v<CPU>,
                  "Native emitter requires fixed CPU field offsets");
    const auto &ir = decoded.instructions;
    auto pos = decoded.end;
    std::vector<uint32_t> code;
    auto emit = [&](uint32_t x) { code.push_back(x); };
    auto mov = [&](int dst, int src) { emit(0x2a0003e0u | (src << 16) | dst); };
    auto extract = [&](int dst, int src, int width, int shift) {
        if (width == 4)
            mov(dst, src);
        else
            emit(0x53000000u | (shift << 16) | ((shift + width * 8 - 1) << 10) | (src << 5) |
                 dst); // ubfx: includes the legacy high-byte registers.
    };
    auto insert = [&](int dst, int src, int width, int shift) {
        if (width == 4)
            mov(dst, src);
        else
            emit(0x33000000u | (((32 - shift) & 31) << 16) | ((width * 8 - 1) << 10) | (src << 5) |
                 dst); // bfi: preserve every other bit of the guest GPR.
    };
    auto imm = [&](int dst, uint32_t v) {
        emit(0x52800000u | ((v & 65535) << 5) | dst);
        if (v >> 16)
            emit(0x72a00000u | ((v >> 16) << 5) | dst);
    };
    auto load = [&](int dst, int base, unsigned off) {
        emit(0xb9400000u | ((off / 4) << 10) | (base << 5) | dst);
    };
    auto store = [&](int src, int base, unsigned off) {
        emit(0xb9000000u | ((off / 4) << 10) | (base << 5) | src);
    };
    // Keep referenced guest GPRs in w20..w27. Untouched registers do not need
    // loading, spilling, or host callee-save traffic on every block entry.
    unsigned usedRegisters = 0;
    for (auto i : ir) {
        if (i.kind == Kind::Nop || i.kind == Kind::Branch)
            continue;
        if (!(i.kind == Kind::Memory && (i.immediate || i.nearReturn)))
            usedRegisters |= 1u << i.dst;
        if (i.kind == Kind::Memory || i.kind == Kind::Lea) {
            if (i.memory.base >= 0)
                usedRegisters |= 1u << i.memory.base;
            if (i.memory.index >= 0)
                usedRegisters |= 1u << i.memory.index;
        } else if (!i.immediate || i.kind == Kind::Multiply)
            usedRegisters |= 1u << i.src;
    }
    bool fastMemory = false, fastStore = false;
    for (auto i : ir)
        if (i.kind == Kind::Memory && (!i.memory.store || !block.watched)) {
            fastMemory = true;
            fastStore |= i.memory.store;
        }
    // x28 is free of guest GPR assignments. An unused guest-register slot can
    // also hold the bitmap base; both survive calls to the guarded helper.
    int bitmapRegister = -1;
    if (fastStore)
        for (int reg = 7; reg >= 0; reg--)
            if (!(usedRegisters & (1u << reg))) {
                bitmapRegister = 20 + reg;
                break;
            }
    auto savePair = [&](int pair) {
        return pair == 0 || (usedRegisters & (3u << (pair * 2 - 1))) || (pair == 4 && fastMemory) ||
               bitmapRegister == 19 + pair * 2 || bitmapRegister == 20 + pair * 2;
    };
    emit(0xa9b97bfdu); // stp x29,x30,[sp,#-112]!
    emit(0x910003fdu); // mov x29,sp: keep a valid host frame chain for diagnostics.
    for (int k = 0; k < 5; k++)
        if (savePair(k))
            emit(0xa9000000u | (((16 + k * 16) / 8) << 15) | ((20 + k * 2) << 10) | (31 << 5) |
                 (19 + k * 2));
    emit(0xaa0003f3u); // mov x19,x0
    for (int reg = 0; reg < 8; reg++)
        if (usedRegisters & (1u << reg))
            load(20 + reg, 19, offsetof(CPU, r) + reg * 4);
    auto pointer = [&](int reg, uintptr_t value) {
        emit(0xd2800000u | ((value & 65535) << 5) | reg);
        for (unsigned part = 1; part < 4; part++)
            emit(0xf2800000u | (part << 21) | (((value >> (part * 16)) & 65535) << 5) | reg);
    };
    if (fastMemory)
        pointer(28, reinterpret_cast<uintptr_t>(block.ram));
    if (bitmapRegister >= 0)
        pointer(bitmapRegister, reinterpret_cast<uintptr_t>(block.codeBits));
    auto cpuAddress = [&](int reg, unsigned offset) {
        // CPU-local arrays need at most two ADD immediates, rather than four
        // MOV-wide instructions materializing an absolute host address.
        unsigned high = offset >> 12, low = offset & 4095;
        if (high)
            emit(0x91400000u | (high << 10) | (19 << 5) | reg);
        if (low || !high)
            emit(0x91000000u | (low << 10) | ((high ? reg : 19) << 5) | reg);
    };
    auto call = [&](uintptr_t address) {
        pointer(16, address);
        emit(0xd63f0200u);
    };
    auto bits = [&](int dst, int src, unsigned first, unsigned count) {
        emit(0x53000000u | (first << 16) | ((first + count - 1) << 10) |
             (src << 5) | dst);
    };
    auto flagBit = [&](int src, unsigned bit) {
        emit(0x33000000u | (((32 - bit) & 31) << 16) | (src << 5) | 12);
    };
    // Return the non-inverted (even Jcc) predicate in w13.
    auto conditionValue = [&](int condition, bool loadFlags) {
        if (loadFlags)
            load(12, 19, offsetof(CPU, flags));
        unsigned base = condition & 14;
        unsigned bit = base == 0 ? 11 : base == 2 || base == 6 ? 0
                                       : base == 4 ? 6 : base == 8 ? 7 : 2;
        if (base >= 12) {
            bits(13, 12, 7, 1);
            bits(14, 12, 11, 1);
            emit(0x4a000000u | (14 << 16) | (13 << 5) | 13);
        } else
            bits(13, 12, bit, 1);
        if (base == 6 || base == 14) {
            bits(14, 12, 6, 1);
            emit(0x2a000000u | (14 << 16) | (13 << 5) | 13);
        }
    };
    auto finishFlags = [&](int last, int condition, uint32_t target) {
        if (last < 0 && condition < 0)
            return;
        // Materialize supported ALU flags without a host helper. Match the
        // interpreter's deterministic undefined-AF value for logical ops too.
        bool logical = last >= 0 &&
                       (ir[last].op == 1 || ir[last].op == 4 || ir[last].op == 6);
        bool arithmetic = last >= 0 &&
                          (ir[last].op == 0 || ir[last].op == 5 || ir[last].op == 7 ||
                           ir[last].op == 8 || ir[last].op == 9);
        if (logical || arithmetic) {
            load(9, 31, 96);
            load(10, 31, 100);
            auto op = ir[last].op;
            bool addition = op == 0 || op == 8, preserveCarry = op >= 8;
            if (arithmetic) {
                extract(9, 9, ir[last].w, 0);
                extract(10, 10, ir[last].w, 0);
                // Zero-extended inputs allow a full 33-bit sum for 32-bit CF.
                emit((addition ? 0x8b000000u : 0x4b000000u) |
                     (10 << 16) | (9 << 5) | 11);
            } else
                emit((op == 1 ? 0x2a000000u : op == 4 ? 0x0a000000u : 0x4a000000u) |
                     (10 << 16) | (9 << 5) | 11);
            load(12, 19, offsetof(CPU, flags));
            imm(14, ~((preserveCarry ? 0 : CF) | PF | AF | ZF | SF | OF));
            emit(0x0a000000u | (14 << 16) | (12 << 5) | 12);
            imm(14, 2);
            emit(0x2a000000u | (14 << 16) | (12 << 5) | 12);
            if (arithmetic && !preserveCarry) {
                if (addition) {
                    auto bit = ir[last].w * 8;
                    emit(0xd3400000u | (bit << 16) | (bit << 10) | (11 << 5) | 13);
                } else {
                    emit(0x6b00001fu | (10 << 16) | (9 << 5)); // cmp w9,w10
                    emit(0x1a9f27edu); // cset w13,lo: x86 CF means borrow
                }
                flagBit(13, 0);
            }
            extract(11, 11, ir[last].w, 0);
            if (arithmetic) {
                emit(0x4a000000u | (10 << 16) | (9 << 5) | 13);
                if (addition)
                    emit(0x2a2d03edu); // mvn w13,w13
                emit(0x4a000000u | (11 << 16) | (9 << 5) | 14);
                emit(0x0a000000u | (14 << 16) | (13 << 5) | 13);
                bits(13, 13, ir[last].w * 8 - 1, 1);
                flagBit(13, 11);
            }
            emit(0x4a000000u | (10 << 16) | (9 << 5) | 13);
            emit(0x4a000000u | (11 << 16) | (13 << 5) | 13);
            bits(13, 13, 4, 1);
            flagBit(13, 4);
            emit(0x7100017fu); // cmp w11,#0
            emit(0x1a9f17edu); // cset w13,eq
            flagBit(13, 6);
            bits(13, 11, ir[last].w * 8 - 1, 1);
            flagBit(13, 7);
            bits(14, 11, 0, 8);
            for (unsigned shift : {4u, 2u, 1u})
                emit(0x4a400000u | (14 << 16) | (shift << 10) | (14 << 5) | 14);
            emit(0x2a2e03eeu); // mvn w14,w14: even parity sets PF
            flagBit(14, 2);
            store(12, 19, offsetof(CPU, flags));
        }
        if (logical || arithmetic || last < 0) {
            if (condition < 0)
                return;
            // Evaluate all sixteen Jcc predicates from materialized flags.
            // Only caller-save temporaries are used, just like nativeFinish.
            conditionValue(condition, last < 0);
            auto skip = code.size();
            emit((condition & 1 ? 0x35000000u : 0x34000000u) | 13);
            imm(9, target);
            store(9, 19, offsetof(CPU, ip));
            code[skip] |= uint32_t(code.size() - skip) << 5;
            return;
        }
        emit(0xaa1303e0u);
        imm(1, last >= 0 ? ir[last].op : -1);
        if (last >= 0) {
            load(2, 31, 96);
            load(3, 31, 100);
        }
        imm(4, last >= 0 ? ir[last].w : 4);
        imm(5, condition);
        imm(6, target);
        call(reinterpret_cast<uintptr_t>(&nativeFinish));
    };
    auto neededFlags = jit_detail::analyzeFlagLiveness(ir);
    block.memory = std::make_shared<std::vector<NativeMemoryMove>>();
    block.memory->reserve(ir.size());
    for (auto i : ir)
        if (i.kind == Kind::Memory)
            block.memory->push_back(i.memory);
    size_t memoryIndex = 0;
    unsigned dirtyRegisters = 0;
    auto flushRegisters = [&](unsigned dirty) {
        for (int reg = 0; reg < 8; reg++)
            if (dirty & (1u << reg))
                store(20 + reg, 19, offsetof(CPU, r) + reg * 4);
    };
    std::vector<size_t> exits;
    int last = -1;
    for (size_t n = 0; n < ir.size(); n++) {
        auto i = ir[n];
        if (i.kind == Kind::Nop)
            continue;
        if (i.kind == Kind::Branch) {
            if (i.dst >= 0 && n + 1 < ir.size()) {
                finishFlags(last, -1, 0);
                last = -1;
                conditionValue(i.dst, true);
                auto expected = code.size();
                bool nonzero = i.predictedTaken != bool(i.dst & 1);
                emit((nonzero ? 0x35000000u : 0x34000000u) | 13);
                // The unexpected outcome returns with the prefix's exact GPRs,
                // materialized flags, instruction count and branch destination.
                flushRegisters(dirtyRegisters);
                imm(9, i.predictedTaken ? i.end : i.imm);
                store(9, 19, offsetof(CPU, ip));
                imm(0, n + 1);
                exits.push_back(code.size());
                emit(0x14000000u);
                code[expected] |= uint32_t(code.size() - expected) << 5;
            }
            continue;
        }
        // A register-only CMP/TEST has no other architectural result. Its
        // instruction still contributes to the block's guest instruction count.
        if (!neededFlags[n] && (i.kind == Kind::Test || (i.kind == Kind::Alu && i.op == 7)))
            continue;
        if (i.kind == Kind::Shift) {
            if (neededFlags[n]) {
                finishFlags(last, -1, 0);
                last = -1;
            }
            unsigned width = i.w * 8, count = i.imm;
            bool left = i.op == 4 || i.op == 6, arithmetic = i.op == 7;
            extract(9, 20 + i.dst, i.w, i.dstShift);
            if (arithmetic) {
                emit(0x13000000u | ((width - 1) << 10) | (9 << 5) | 11);
                emit(0x13007c00u | (count << 16) | (11 << 5) | 11); // asr w11,#count
            } else if (count >= width)
                imm(11, 0);
            else if (left)
                emit(0x53000000u | ((32 - count) << 16) | ((31 - count) << 10) |
                     (9 << 5) | 11); // lsl w11,w9,#count
            else
                emit(0x53007c00u | (count << 16) | (9 << 5) | 11); // lsr
            extract(11, 11, i.w, 0);
            if (neededFlags[n]) {
                load(12, 19, offsetof(CPU, flags));
                imm(14, ~(CF | SF | ZF | PF | (count == 1 ? OF : 0)));
                emit(0x0a000000u | (14 << 16) | (12 << 5) | 12);
                if (arithmetic || count <= width)
                    bits(13, 9, left ? width - count : std::min(count, width) - 1, 1);
                else
                    imm(13, 0);
                flagBit(13, 0);
                if (count == 1 && !arithmetic) {
                    bits(14, left ? 11 : 9, width - 1, 1);
                    if (left)
                        emit(0x4a000000u | (14 << 16) | (13 << 5) | 13);
                    else
                        mov(13, 14);
                    flagBit(13, 11);
                }
                emit(0x7100017fu); // cmp w11,#0
                emit(0x1a9f17edu); // cset w13,eq
                flagBit(13, 6);
                bits(13, 11, width - 1, 1);
                flagBit(13, 7);
                bits(14, 11, 0, 8);
                for (unsigned shift : {4u, 2u, 1u})
                    emit(0x4a400000u | (14 << 16) | (shift << 10) | (14 << 5) | 14);
                emit(0x2a2e03eeu); // even parity
                flagBit(14, 2);
                store(12, 19, offsetof(CPU, flags));
            }
            insert(20 + i.dst, 11, i.w, i.dstShift);
            dirtyRegisters |= 1u << i.dst;
            continue;
        }
        if (i.kind == Kind::Multiply) {
            if (neededFlags[n]) {
                finishFlags(last, -1, 0);
                last = -1;
            }
            unsigned left = 20 + (i.immediate ? i.src : i.dst);
            emit(0x13000000u | ((i.w * 8 - 1) << 10) | (left << 5) | 11);
            if (i.immediate) {
                imm(12, i.imm);
                emit(0x13000000u | ((i.w * 8 - 1) << 10) | (12 << 5) | 12);
            } else
                emit(0x13000000u | ((i.w * 8 - 1) << 10) | ((20 + i.src) << 5) | 12);
            emit(0x9b207c00u | (12 << 16) | (11 << 5) | 13); // smull x13,w11,w12
            if (neededFlags[n]) {
                emit(0x93400000u | ((i.w * 8 - 1) << 10) | (13 << 5) | 14);
                emit(0xeb00001fu | (14 << 16) | (13 << 5)); // cmp full,sign-extended low
                emit(0x1a9f07ebu); // cset w11,ne: signed overflow sets both CF and OF
                load(12, 19, offsetof(CPU, flags));
                imm(14, ~(CF | OF));
                emit(0x0a000000u | (14 << 16) | (12 << 5) | 12);
                for (unsigned bit : {0u, 11u})
                    emit(0x33000000u | (((32 - bit) & 31) << 16) | (11 << 5) | 12);
                store(12, 19, offsetof(CPU, flags));
            }
            insert(20 + i.dst, 13, i.w, 0);
            dirtyRegisters |= 1u << i.dst;
            continue;
        }
        if (i.kind == Kind::Exchange) {
            // Capture both operands before either insert: AL/AH and other
            // partial-register aliases can share the same cached host GPR.
            extract(11, 20 + i.dst, i.w, i.dstShift);
            extract(12, 20 + i.src, i.w, i.srcShift);
            insert(20 + i.dst, 12, i.w, i.dstShift);
            insert(20 + i.src, 11, i.w, i.srcShift);
            dirtyRegisters |= (1u << i.dst) | (1u << i.src);
            continue;
        }
        if (i.kind == Kind::CounterBranch) {
            extract(11, 20 + CX, i.w, 0);
            if (i.op == 0xe2) {
                emit(0x5100056bu); // sub w11,w11,#1, without changing NZCV
                insert(20 + CX, 11, i.w, 0);
                dirtyRegisters |= 1u << CX;
            }
            imm(9, pos);
            store(9, 19, offsetof(CPU, ip));
            auto fallthrough = code.size();
            emit(i.op == 0xe2 ? 0x3400000bu : 0x3500000bu); // cbz/cbnz w11
            imm(9, i.imm);
            store(9, 19, offsetof(CPU, ip));
            code[fallthrough] |= uint32_t(code.size() - fallthrough) << 5;
            continue;
        }
        if (i.kind == Kind::Lea) {
            imm(10, i.memory.displacement);
            if (i.memory.base >= 0)
                emit(0x0b000000u | ((20 + i.memory.base) << 16) | (10 << 5) | 10);
            if (i.memory.index >= 0)
                emit(0x0b000000u | ((20 + i.memory.index) << 16) | (i.memory.scale << 10) |
                     (10 << 5) | 10);
            if (!i.memory.address32)
                extract(10, 10, 2, 0);
            insert(20 + i.dst, 10, i.w, 0);
            dirtyRegisters |= 1u << i.dst;
            continue;
        }
        if (i.kind == Kind::Memory) {
            const auto accessWidth = i.memory.width;
            auto beforeLoad = dirtyRegisters;
            auto metadata = &(*block.memory)[memoryIndex++];
            auto finishMove = [&]() {
                if (i.compareMemory) {
                    // w10 contains the successful read. Only now may the new
                    // flags replace the preceding instruction's restart state.
                    if (i.immediate)
                        imm(12, i.imm & (i.w == 1 ? 255 : i.w == 2 ? 65535 : UINT32_MAX));
                    else
                        extract(12, 20 + i.dst, i.w, i.dstShift);
                    if (i.reverse) {
                        store(12, 31, 96);
                        store(10, 31, 100);
                    } else {
                        store(10, 31, 96);
                        store(12, 31, 100);
                    }
                    return;
                }
                // Change SP only after a successful access. POP SP/ESP writes
                // its destination after the increment, including mixed widths.
                if (i.stackDelta) {
                    auto amount = unsigned(i.stackDelta < 0 ? -i.stackDelta : i.stackDelta);
                    if (amount < 4096)
                        emit((i.stackDelta < 0 ? 0x51000000u : 0x11000000u) | (amount << 10) |
                             ((20 + SP) << 5) | 11);
                    else {
                        imm(12, amount);
                        emit(0x0b000000u | (12 << 16) | ((20 + SP) << 5) | 11);
                    }
                    insert(20 + SP, 11, block.stack32 ? 4 : 2, 0);
                    dirtyRegisters |= 1u << SP;
                }
                if (i.nearReturn) {
                    // Commit the dynamic destination only after a successful
                    // stack read. Fault/MMIO replay retains the original IP.
                    store(10, 19, offsetof(CPU, ip));
                } else if (!i.memory.store) {
                    if (i.signedSource)
                        emit(0x13000000u | ((i.sourceWidth * 8 - 1) << 10) | (10 << 5) | 10);
                    insert(20 + i.dst, 10, i.w, i.dstShift);
                    dirtyRegisters |= 1u << i.dst;
                }
            };
            imm(2, i.memory.displacement);
            if (i.memory.base >= 0)
                emit(0x0b000000u | ((20 + i.memory.base) << 16) | (2 << 5) | 2);
            if (i.memory.index >= 0)
                emit(0x0b000000u | ((20 + i.memory.index) << 16) | (i.memory.scale << 10) |
                     (2 << 5) | 2);
            if (!i.memory.address32)
                emit(0x53003c00u | (2 << 5) | 2);
            // Keep w2 (the original segment offset) intact for the slow helper.
            // Inline only side-effect-free TLB hits and ordinary RAM. Misses,
            // dirty-bit updates, code writes and devices retain the precise
            // existing helper/replay path below.
            size_t fastSuccess = size_t(-1);
            if ((!i.memory.store || !block.watched) && block.ramSize >= unsigned(accessWidth) &&
                block.ramSize <= UINT32_MAX) {
                std::vector<size_t> guards;
                auto guard = [&](uint32_t instruction) {
                    guards.push_back(code.size());
                    emit(instruction);
                };
                auto compareImmediate = [&](int reg, uint32_t value) {
                    if (value < 4096)
                        emit(0x7100001fu | (value << 10) | (reg << 5));
                    else {
                        imm(15, value);
                        emit(0x6b00001fu | (15 << 16) | (reg << 5));
                    }
                };
                auto segmentOffset = offsetof(CPU, s) + i.memory.segment * sizeof(Segment);
                if ((block.cr0 & 1) && !block.vm) {
                    emit(0x39400000u | ((segmentOffset + offsetof(Segment, unusable)) << 10) |
                         (19 << 5) | 9); // ldrb w9,[x19,#unusable]
                    guard(0x35000009u);  // cbnz w9,slow
                }
                load(9, 19, segmentOffset + offsetof(Segment, base));
                emit(0x0b020129u); // add w9,w9,w2 (linear address, wraps at 32 bits)
                emit(0x12002d2au); // and w10,w9,#4095
                compareImmediate(10, 4096 - accessWidth);
                guard(0x54000008u); // b.hi slow (cross-page access)
                if (block.cr0 & 0x80000000u) {
                    emit(0x530c7d2au); // lsr w10,w9,#12 (page tag)
                    emit(0x4a4a294bu); // eor w11,w10,w10,lsr #10
                    emit(0x1200256bu); // and w11,w11,#1023
                    emit(0x0b0b096bu); // add w11,w11,w11,lsl #2
                    static_assert(sizeof(CPU::TLBEntry) == 20);
                    static_assert(offsetof(CPU::TLBEntry, permissions) == 16);
                    static_assert(offsetof(CPU::TLBEntry, dirty) == 17);
                    cpuAddress(12, offsetof(CPU, tlb));
                    emit(0x8b2b498cu); // add x12,x12,w11,uxtw #2
                    load(13, 12, offsetof(CPU::TLBEntry, epoch));
                    cpuAddress(14, offsetof(CPU, tlbEpoch) & ~4095u);
                    load(14, 14, offsetof(CPU, tlbEpoch) & 4095u);
                    emit(0x6b0e01bfu);  // cmp w13,w14
                    guard(0x54000001u); // b.ne slow
                    load(13, 12, offsetof(CPU::TLBEntry, tag));
                    emit(0x6b0a01bfu); // cmp w13,w10
                    guard(0x54000001u);
                    bool user = block.vm || ((block.cr0 & 1) && (cpu.s[CS].selector & 3) == 3);
                    if (user) {
                        load(13, 19, offsetof(CPU, systemAccess));
                        guard(0x3500000du); // cbnz w13,slow (supervisor override)
                        emit(0x3940418du);  // ldrb w13,[x12,#permissions]
                        emit(0x121f05adu);  // and w13,w13,#6
                        compareImmediate(13, i.memory.store ? 6 : 4);
                        guard(i.memory.store ? 0x54000001u : 0x54000003u); // ne / lo
                    }
                    if (i.memory.store) {
                        emit(0x3940458du);  // ldrb w13,[x12,#dirty]
                        guard(0x3400000du); // cbz w13,slow
                    }
                    emit(0x12002d2eu); // and w14,w9,#4095
                    load(13, 12, offsetof(CPU::TLBEntry, base));
                    emit(0x2a0e01a9u); // orr w9,w13,w14
                }
                if (!block.a20) {
                    imm(10, 1u << 20);
                    emit(0x0a2a0129u); // bic w9,w9,w10
                }
                if (!(block.ramSize & 4095) && std::has_single_bit(uint32_t(block.ramSize))) {
                    // The earlier page-boundary guard proves the entire access
                    // fits if its first byte lies in page-aligned RAM.
                    auto shift = std::countr_zero(uint32_t(block.ramSize));
                    emit(0x53007c00u | (shift << 16) | (9 << 5) | 10);
                    guard(0x3500000au); // cbnz w10,slow (outside RAM)
                } else {
                    compareImmediate(9, uint32_t(block.ramSize - accessWidth));
                    guard(0x54000008u); // b.hi slow (outside RAM)
                }
                emit(0x53107d2au); // lsr w10,w9,#16
                compareImmediate(10, 10);
                guard(0x54000000u); // b.eq slow (VGA aperture)
                if (i.memory.store) {
                    emit(0x1200152au); // and w10,w9,#63
                    compareImmediate(10, 64 - accessWidth);
                    guard(0x54000008u); // b.hi slow (two code bitmap words)
                    emit(0x53067d2au);  // lsr w10,w9,#6
                    if (bitmapRegister >= 0)
                        emit(0x8b2a4c00u | (bitmapRegister << 5) | 12);
                    else {
                        pointer(12, reinterpret_cast<uintptr_t>(block.codeBits));
                        emit(0x8b2a4d8cu); // add x12,x12,w10,uxtw #3
                    }
                    emit(0xf940018au);  // ldr x10,[x12]
                    guard(0xb500000au); // cbnz x10,slow (possibly translated bytes)
                }
                emit(0x8b29438bu); // add x11,x28,w9,uxtw
                if (i.memory.store) {
                    int source = 20 + i.dst;
                    if (i.immediate) {
                        imm(10, i.imm);
                        source = 10;
                    } else if (i.dstShift) {
                        extract(10, source, accessWidth, i.dstShift);
                        source = 10;
                    }
                    emit((accessWidth == 4   ? 0xb9000000u
                          : accessWidth == 2 ? 0x79000000u
                                     : 0x39000000u) |
                         (11 << 5) | source);
                } else {
                    emit((accessWidth == 4   ? 0xb9400000u
                          : accessWidth == 2 ? 0x79400000u
                                     : 0x39400000u) |
                         (11 << 5) | 10);
                }
                finishMove();
                fastSuccess = code.size();
                emit(0x14000000u);
                for (auto at : guards)
                    code[at] |= uint32_t(code.size() - at) << 5;
            }
            emit(0xaa1303e0u);
            pointer(1, reinterpret_cast<uintptr_t>(metadata));
            if (i.immediate)
                imm(3, i.imm);
            else if (i.dstShift)
                extract(3, 20 + i.dst, i.w, i.dstShift);
            else
                mov(3, 20 + i.dst);
            imm(4, block.page);
            imm(5, block.generation);
            call(reinterpret_cast<uintptr_t>(&nativeMemory));
            auto fallback = code.size();
            emit(0x34000000u); // cbz w0, fallback; guest registers are still cached.
            if (!i.memory.store) {
                load(10, 19, offsetof(CPU, nativeValue));
            }
            finishMove();
            emit(0x7100041fu); // cmp w0,#1: completed and code unchanged.
            auto continuation = code.size();
            emit(0x54000000u); // b.eq continuation
            // Successful memory access changed code: commit through this instruction.
            flushRegisters(dirtyRegisters);
            finishFlags(i.compareMemory ? int(n) : last, -1, 0);
            if (!i.nearReturn) {
                imm(9, i.memory.end);
                store(9, 19, offsetof(CPU, ip));
            }
            imm(0, n + 1);
            exits.push_back(code.size());
            emit(0x14000000u);
            code[fallback] |= uint32_t(code.size() - fallback) << 5;
            // Declined access: preserve the pre-move destination and replay it.
            flushRegisters(beforeLoad);
            finishFlags(last, -1, 0);
            imm(9, i.memory.begin);
            store(9, 19, offsetof(CPU, ip));
            imm(0, n);
            exits.push_back(code.size());
            emit(0x14000000u);
            code[continuation] |= uint32_t(code.size() - continuation) << 5;
            if (fastSuccess != size_t(-1))
                code[fastSuccess] |= uint32_t(code.size() - fastSuccess);
            if (i.compareMemory)
                last = n;
            continue;
        }
        if ((i.kind == Kind::Alu || i.kind == Kind::Test) && neededFlags[n]) {
            if (i.kind == Kind::Alu && i.op >= 8)
                finishFlags(last, -1, 0);
            last = n;
        }
        int dst = 20 + i.dst;
        extract(11, dst, i.w, i.dstShift);
        if (i.immediate)
            imm(12, i.imm & (i.w == 1 ? 255 : i.w == 2 ? 65535 : 0xffffffff));
        else {
            extract(12, 20 + i.src, i.sourceWidth ? i.sourceWidth : i.w, i.srcShift);
            if (i.signedSource)
                emit(0x13000000u | ((i.sourceWidth * 8 - 1) << 10) | (12 << 5) | 12);
        }
        if (neededFlags[n]) {
            store(11, 31, 96);
            store(12, 31, 100);
        }
        if (i.kind == Kind::Move)
            mov(10, 12);
        else {
            uint32_t op = i.op == 0 || i.op == 8 ? 0x0b000000
                          : i.op == 1 ? 0x2a000000
                          : i.op == 4 ? 0x0a000000
                          : i.op == 6 ? 0x4a000000
                                      : 0x4b000000;
            emit(op | (12 << 16) | (11 << 5) | 10);
        }
        if (i.kind == Kind::Test || (i.kind == Kind::Alu && i.op == 7))
            continue;
        dirtyRegisters |= 1u << i.dst;
        insert(dst, 10, i.w, i.dstShift);
    }
    flushRegisters(dirtyRegisters);
    bool branch = ir.back().kind == Kind::Branch;
    if (!ir.back().nearReturn && ir.back().kind != Kind::CounterBranch) {
        imm(9, ir.back().nearCall ? ir.back().target
                                 : branch ? (ir.back().dst < 0 ? ir.back().imm : ir.back().end)
                                          : pos);
        store(9, 19, offsetof(CPU, ip));
    }
    finishFlags(last, branch ? ir.back().dst : -1, branch ? ir.back().imm : 0);
    imm(0, ir.size());
    for (auto at : exits)
        code[at] |= uint32_t(code.size() - at) & 0x03ffffffu;
    for (int k = 0; k < 5; k++)
        if (savePair(k))
            emit(0xa9400000u | (((16 + k * 16) / 8) << 15) | ((20 + k * 2) << 10) | (31 << 5) |
                 (19 + k * 2));
    emit(0xa8c77bfdu);
    emit(0xd65f03c0u); // ldp x29,x30,[sp],#112; ret
    size_t bytes = code.size() * 4;
    if (bytes > capacity)
        return block; // Never copy an oversized block beyond the executable arena.
    if (used + bytes > capacity) {
        blocks.clear();
        cacheRecycles++;
        fast = {};
        used = 0;
    }
    pthread_jit_write_protect_np(0);
    std::memcpy(arena + used, code.data(), bytes);
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate(arena + used, bytes);
    block.fn = reinterpret_cast<Function>(arena + used);
    used = (used + bytes + 15) & ~size_t(15);
    compiledBlocks++;
    return block;
}
} // namespace pc
#endif
