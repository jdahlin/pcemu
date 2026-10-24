#include "jit.hpp"
#include "profile.hpp"
#include <bit>
#include <cstddef>
#include <cstring>
#include <type_traits>
#if defined(__APPLE__) && defined(__aarch64__)
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <sys/mman.h>
#endif
namespace pc {
static void nativeFinish(CPU *cpu, int op, uint32_t a, uint32_t b, int w, int condition,
                         uint32_t target) {
    if (op >= 8) {
        auto carry = cpu->flags & CF;
        cpu->alu(op == 8 ? 0 : 5, a, b, w);
        cpu->flags = (cpu->flags & ~CF) | carry;
    } else if (op >= 0)
        cpu->alu(op, a, b, w);
    if (condition >= 0 && cpu->condition(condition))
        cpu->ip = target;
}
// No C++ exception can unwind through generated ARM64 frames. Decline before
// touching devices or guest data; the dispatcher replays that instruction in
// the interpreter with committed GPRs, flags and its original instruction IP.
static uint32_t nativeMemory(CPU *cpu, const NativeMemoryMove *op, uint32_t offset, uint32_t value,
                             uint32_t codePage, uint32_t codeGeneration) {
    cpu->nativeMemoryCalls++;
    auto previousCR2 = cpu->cr[2];
    try {
        if (op->width != 1 && op->width != 2 && op->width != 4) {
            cpu->nativeFallback = true;
            return 0;
        }
        if (op->store && cpu->watchWrite) {
            cpu->nativeFallback = true;
            return 0;
        }
        auto linear = cpu->address(op->segment, offset);
        if ((linear & 4095) + op->width > 4096) {
            cpu->nativeFallback = true;
            return 0;
        }
        auto physical = cpu->physical(linear, op->store);
        if (uint64_t(physical) + op->width > cpu->mem.size() ||
            (physical < 0xb0000 && uint64_t(physical) + op->width > 0xa0000)) {
            cpu->nativeFallback = true;
            return 0;
        }
        if (op->store) {
            if (op->width == 4)
                std::memcpy(cpu->mem.data() + physical, &value, 4);
            else if (op->width == 2)
                std::memcpy(cpu->mem.data() + physical, &value, 2);
            else
                cpu->mem[physical] = uint8_t(value);
            cpu->written(physical, op->width);
        } else {
            value = 0;
            if (op->width == 4)
                std::memcpy(&value, cpu->mem.data() + physical, 4);
            else if (op->width == 2)
                std::memcpy(&value, cpu->mem.data() + physical, 2);
            else
                value = cpu->mem[physical];
            cpu->nativeValue = value;
        }
        return cpu->generation[codePage] == codeGeneration ? 1 : 2;
    } catch (...) {
        cpu->cr[2] = previousCR2;
        cpu->nativeFallback = true;
        return 0;
    }
}
Jit::Jit(CPU &c, bool en, unsigned heat, size_t arenaBytes)
    : cpu(c), capacity(arenaBytes), warmup(heat) {
    if (capacity < 65536)
        throw std::invalid_argument("JIT arena must be at least 64 KiB");
#if defined(__APPLE__) && defined(__aarch64__)
    if (en) {
        arena = static_cast<uint8_t *>(mmap(nullptr, capacity, PROT_READ | PROT_WRITE | PROT_EXEC,
                                            MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0));
        if (arena == MAP_FAILED) {
            arena = nullptr;
            throw std::runtime_error("MAP_JIT failed");
        }
        active = true;
    }
#else
    (void)en;
#endif
}
Jit::~Jit() {
#if defined(__APPLE__) && defined(__aarch64__)
    if (arena)
        munmap(arena, capacity);
#endif
}
void Jit::clear() {
    blocks.clear();
    fast = {};
    used = 0;
    nativeInstructions = compiledBlocks = cacheHits = cacheRecycles = 0;
}
uint32_t Jit::run() {
    profile::event(profile::Dispatch);
    if (cpu.halted)
        return 0;
    if (!active ||
        (cpu.interruptBlockedThrough && cpu.instructions <= cpu.interruptBlockedThrough) ||
        (cpu.flags & TF)) {
        profile::event(profile::InterpretedDispatch);
        cpu.step();
        return 1;
    }
    // Include segment base and mode: real-mode aliases may share linear addresses
    // but have different IPs and operand sizes. Paged entries validate translation
    // and access permission before using cached native code.
    uint64_t key = uint64_t(cpu.s[CS].selector) << 48 | uint64_t(cpu.s[CS].big) << 47 | cpu.ip;
    auto &entry = fast[(cpu.ip ^ (cpu.ip >> 12) ^ (cpu.s[CS].selector * 31u)) & 4095];
    Block *cached = entry.key == key ? entry.block : nullptr;
    if (cached)
        profile::event(profile::FastHit);
    if (!cached) {
        auto it = blocks.find(key);
        if (it != blocks.end()) {
            cached = &it->second;
            profile::event(profile::MapHit);
        } else
            profile::event(profile::LookupMiss);
    }
    uint32_t currentPage = 0;
    bool checkedPage = false;
    if ((cpu.cr[0] & 0x80000000u) && cached && (cached->fn || cached->tlbEpoch != cpu.tlbEpoch)) {
        try {
            currentPage = cpu.physical(cpu.address(CS, cpu.ip)) >> 12;
            checkedPage = true;
        } catch (const Fault &) {
            cpu.step(); // Deliver #PF with the interpreter's restartable frame.
            return 1;
        }
    }
    if (!cached || cached->executionEpoch != cpu.executionEpoch ||
        cached->page >= cpu.generation.size() ||
        cached->generation != cpu.generation[cached->page] || cached->cr0 != cpu.cr[0] ||
        cached->cr3 != cpu.cr[3] || cached->csBase != cpu.s[CS].base || cached->a20 != cpu.a20 ||
        cached->vm != bool(cpu.flags & 0x20000) ||
        // These guards protect addresses and assumptions embedded in native
        // code. Negative and warming entries execute CPU::step(), which reads
        // the current buffers, watch callback and stack mode itself. Keep code
        // identity checks above immediate so changed code can be translated.
        (cached->fn &&
         (cached->watched != bool(cpu.watchWrite) ||
          (cached->usesStack && cached->stack32 != cpu.s[SS].big) ||
          cached->ram != cpu.mem.data() || cached->ramSize != cpu.mem.size() ||
          cached->codeBits != cpu.codeUsed.data())) ||
        (checkedPage && cached->page != currentPage)) {
        try {
            profile::event(profile::MetadataRebuild);
            auto metadata = compile(false);
            cached = &blocks.insert_or_assign(key, metadata).first->second;
        } catch (const Fault &) {
            cpu.step();
            return 1;
        }
    }
    // A TLB flush need not discard decoded code when fetch permissions were
    // rechecked and the same physical instruction page is still mapped.
    cached->tlbEpoch = cpu.tlbEpoch;
    if (!cached->attempted && ++cached->visits >= warmup) {
        try {
            profile::event(profile::Translate);
            auto translated = compile();
            // Compilation can recycle the arena and clear both lookup tables.
            cached = &blocks.insert_or_assign(key, translated).first->second;
        } catch (const Fault &) {
            cpu.step();
            return 1;
        }
    }
    entry = {key, cached};
    auto &b = *cached;
    if (!b.fn) {
        profile::event(profile::InterpretedDispatch);
        if (b.attempted)
            profile::event(profile::NegativeHit);
        cpu.step();
        return 1;
    }
    cacheHits++;
    profile::event(profile::NativeCall);
    cpu.nativeFallback = false;
    auto n = b.fn(&cpu);
    cpu.instructions += n;
    nativeInstructions += n;
    if (cpu.nativeFallback) {
        profile::event(profile::NativeReplay);
        cpu.nativeFallback = false;
        cpu.step();
        return n + 1;
    }
    return n;
}
Jit::Block Jit::compile(bool translate) {
    Block block;
    auto linear = cpu.address(CS, cpu.ip), physical = cpu.physical(linear);
    block.page = physical >> 12;
    block.generation = cpu.generation.at(block.page);
    block.cr0 = cpu.cr[0];
    block.cr3 = cpu.cr[3];
    block.csBase = cpu.s[CS].base;
    block.a20 = cpu.a20;
    block.tlbEpoch = cpu.tlbEpoch;
    block.executionEpoch = cpu.executionEpoch;
    block.vm = cpu.flags & 0x20000;
    block.watched = bool(cpu.watchWrite);
    block.stack32 = cpu.s[SS].big;
    block.ram = cpu.mem.data();
    block.ramSize = cpu.mem.size();
    block.codeBits = cpu.codeUsed.data();
    if (!translate)
        return block;
    block.attempted = true;
#if defined(__APPLE__) && defined(__aarch64__)
    static_assert(std::is_standard_layout_v<CPU>,
                  "Native emitter requires fixed CPU field offsets");
    struct IR {
        int kind, dst, src, op, w, dstShift, srcShift, stackDelta;
        uint32_t imm, end;
        bool immediate;
        bool compareMemory = false, reverse = false, nearCall = false, nearReturn = false;
        unsigned sourceWidth = 0;
        bool signedSource = false;
        uint32_t target = 0;
        NativeMemoryMove memory;
    };
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
            i.kind = -1;
            i.dst = 0;
            i.src = 0;
            if (op == 0xc2 || op == 0xc3) {
                i.kind = 5;
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
                i.kind = 5;
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
                i.kind = 5;
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
                i.kind = 1;
                i.immediate = true;
                i.imm = 1;
                if (op == 0xfe || op == 0xff) {
                    auto m = byte();
                    if ((m >> 6) != 3 || ((m >> 3) & 7) > 1) {
                        pos = begin;
                        break;
                    }
                    i.w = op == 0xfe ? 1 : w;
                    i.dst = m & 7;
                    i.op = 8 + ((m >> 3) & 1);
                } else {
                    i.dst = op & 7;
                    i.op = op < 0x48 ? 8 : 9;
                }
            } else if (op >= 0xb0 && op <= 0xbf) {
                i.kind = 0;
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
                i.kind = 8;
                i.w = op == 0x86 ? 1 : w;
                i.dst = m & 7;
                i.src = (m >> 3) & 7;
            } else if (op >= 0x91 && op <= 0x97) {
                i.kind = 8;
                i.dst = AX;
                i.src = op & 7;
            } else if (op >= 0x88 && op <= 0x8b) {
                i.w = (op & 1) ? w : 1;
                auto m = byte();
                if ((m >> 6) != 3) {
                    i.kind = 5;
                    i.memory = memoryOperand(m, address32, overrideSegment);
                    i.dst = i.memory.reg = (m >> 3) & 7;
                    i.memory.store = !(op & 2);
                } else {
                    i.kind = 0;
                    i.dst = !(op & 2) ? m & 7 : (m >> 3) & 7;
                    i.src = !(op & 2) ? (m >> 3) & 7 : m & 7;
                }
            } else if (op == 0x8d) {
                auto m = byte();
                if ((m >> 6) == 3) {
                    pos = begin;
                    break;
                }
                i.kind = 6; // LEA computes an offset without accessing the segment or RAM.
                i.dst = (m >> 3) & 7;
                i.memory = memoryOperand(m, address32, overrideSegment);
            } else if (op >= 0xa0 && op <= 0xa3) {
                i.kind = 5;
                i.w = (op & 1) ? w : 1;
                i.dst = i.memory.reg = AX;
                i.memory.segment = overrideSegment < 0 ? DS : overrideSegment;
                i.memory.address32 = address32;
                i.memory.displacement = immediate(address32 ? 4 : 2);
                i.memory.store = bool(op & 2);
            } else if (op < 0x40 && (op & 7) <= 5 &&
                       ((op >> 3) == 0 || (op >> 3) == 1 || (op >> 3) == 4 || (op >> 3) == 5 ||
                        (op >> 3) == 6 || (op >> 3) == 7)) {
                i.kind = 1;
                i.w = (op & 1) ? w : 1;
                i.op = op >> 3;
                if ((op & 7) >= 4) {
                    i.dst = AX;
                    i.immediate = true;
                    i.imm = immediate(i.w);
                } else {
                    auto m = byte();
                    if ((m >> 6) != 3) {
                        if (i.op != 7) {
                            pos = begin;
                            break;
                        }
                        i.kind = 5;
                        i.compareMemory = true;
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
                    i.compareMemory = true;
                    i.memory = memoryOperand(m, address32, overrideSegment);
                }
                if (i.op == 2 || i.op == 3) {
                    pos = begin;
                    break;
                }
                i.kind = i.compareMemory ? 5 : 1;
                i.dst = i.compareMemory ? 0 : m & 7;
                i.immediate = true;
                i.imm = op == 0x83 ? uint32_t(int32_t(int8_t(byte()))) : immediate(i.w);
            } else if (op == 0x84 || op == 0x85) {
                i.w = (op & 1) ? w : 1;
                auto m = byte();
                i.op = 4;
                if ((m >> 6) != 3) {
                    i.kind = 5;
                    i.compareMemory = true;
                    i.memory = memoryOperand(m, address32, overrideSegment);
                    i.dst = (m >> 3) & 7;
                } else {
                    i.kind = 2;
                    i.dst = m & 7;
                    i.src = (m >> 3) & 7;
                }
            } else if (op == 0xa8 || op == 0xa9) {
                i.kind = 2;
                i.w = op == 0xa8 ? 1 : w;
                i.op = 4;
                i.dst = AX;
                i.immediate = true;
                i.imm = immediate(i.w);
            } else if (op == 0xf6 || op == 0xf7) {
                i.w = op == 0xf6 ? 1 : w;
                auto m = byte();
                if (m & 0x38) {
                    pos = begin;
                    break;
                }
                i.op = 4;
                i.immediate = true;
                if ((m >> 6) == 3) {
                    i.kind = 2;
                    i.dst = m & 7;
                } else {
                    i.kind = 5;
                    i.compareMemory = true;
                    i.memory = memoryOperand(m, address32, overrideSegment);
                }
                i.imm = immediate(i.w);
            } else if (op == 0xe2 || op == 0xe3) {
                auto displacement = int8_t(byte());
                i.kind = 7;
                i.dst = CX;
                i.immediate = true;
                i.op = op;
                i.imm = (pos + displacement) & (w == 4 ? UINT32_MAX : 65535u);
                // Address size selects CX/ECX; operand size selects the target
                // IP mask. Neither LOOP nor JCXZ changes guest flags.
                i.w = address32 ? 4 : 2;
            } else if (op == 0x90) {
                i.kind = 3;
            } else if (op == 0x0f) {
                auto extended = byte();
                if (extended == 0xb6 || extended == 0xb7 || extended == 0xbe || extended == 0xbf) {
                    auto m = byte();
                    i.kind = (m >> 6) == 3 ? 0 : 5;
                    i.dst = (m >> 3) & 7;
                    i.src = m & 7;
                    if (i.kind == 5)
                        i.memory = memoryOperand(m, address32, overrideSegment);
                    i.sourceWidth = (extended & 1) ? 2 : 1;
                    i.signedSource = extended & 8;
                    if (i.kind == 0 && i.sourceWidth == 1) {
                        i.srcShift = (i.src & 4) ? 8 : 0;
                        i.src &= 3;
                    }
                } else if (extended >= 0x80 && extended <= 0x8f) {
                    auto raw = immediate(w);
                    auto displacement = w == 2 ? int32_t(int16_t(raw)) : int32_t(raw);
                    i.kind = 4;
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
                i.kind = 4;
                i.imm = (pos + displacement) & (w == 4 ? 0xffffffffu : 65535u);
            } else {
                // Unsupported instructions end a block normally. Host stack
                // unwinding here was a measurable whole-application cost.
                pos = begin;
                break;
            }
            i.end = pos;
            if (i.w == 1 && (i.kind <= 2 || i.kind == 5 || i.kind == 8)) {
                i.dstShift = (i.dst & 4) ? 8 : 0;
                i.dst &= 3;
                if (!i.immediate && i.kind != 5) {
                    i.srcShift = (i.src & 4) ? 8 : 0;
                    i.src &= 3;
                }
            }
            if (i.kind == 5) {
                i.memory.width = i.sourceWidth ? i.sourceWidth : i.w;
                i.memory.begin = begin;
                i.memory.end = i.nearCall ? i.target : pos;
            }
            ir.push_back(i);
            if (i.kind == 4 || i.kind == 7 || i.nearCall || i.nearReturn)
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
    // Native entry saves and restores the guest register set. Very short
    // blocks cost more than interpreting them unless explicitly testing lowering.
    const bool shortComparison =
        ir.size() == 2 && ir[1].kind == 4 && ir[1].dst >= 0 &&
        (ir[0].compareMemory || ir[0].kind == 2 ||
         (ir[0].kind == 1 && (ir[0].op == 7 || ir[0].op == 8 || ir[0].op == 9)));
    if (ir.empty() || (warmup && ir.size() < 3 && !shortComparison && ir.back().kind != 7 &&
                      !ir.back().nearCall && !ir.back().nearReturn))
        return block;
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
        if (i.kind == 3 || i.kind == 4)
            continue;
        if (!(i.kind == 5 && (i.immediate || i.nearReturn)))
            usedRegisters |= 1u << i.dst;
        if (i.kind == 5 || i.kind == 6) {
            if (i.memory.base >= 0)
                usedRegisters |= 1u << i.memory.base;
            if (i.memory.index >= 0)
                usedRegisters |= 1u << i.memory.index;
        } else if (!i.immediate)
            usedRegisters |= 1u << i.src;
    }
    bool fastMemory = false, fastStore = false;
    for (auto i : ir)
        if (i.kind == 5 && (!i.memory.store || !block.watched)) {
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
        auto bits = [&](int dst, int src, unsigned first, unsigned count) {
            emit(0x53000000u | (first << 16) | ((first + count - 1) << 10) |
                 (src << 5) | dst);
        };
        auto flagBit = [&](int src, unsigned bit) {
            emit(0x33000000u | (((32 - bit) & 31) << 16) | (src << 5) | 12);
        };
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
            if (last < 0)
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
    std::vector<bool> neededFlags(ir.size());
    int last = -1;
    for (size_t n = 0; n < ir.size(); n++) {
        if (ir[n].kind == 1 && ir[n].op >= 8 && last >= 0)
            neededFlags[last] = true; // INC/DEC consume the preceding CF.
        if (ir[n].kind == 1 || ir[n].kind == 2)
            last = n;
        if (ir[n].kind == 5 && last >= 0) {
            neededFlags[last] = true;
        }
        if (ir[n].compareMemory)
            last = n;
    }
    if (last >= 0)
        neededFlags[last] = true;
    block.memory = std::make_shared<std::vector<NativeMemoryMove>>();
    block.memory->reserve(ir.size());
    for (auto i : ir)
        if (i.kind == 5)
            block.memory->push_back(i.memory);
    size_t memoryIndex = 0;
    unsigned dirtyRegisters = 0;
    auto flushRegisters = [&](unsigned dirty) {
        for (int reg = 0; reg < 8; reg++)
            if (dirty & (1u << reg))
                store(20 + reg, 19, offsetof(CPU, r) + reg * 4);
    };
    std::vector<size_t> exits;
    last = -1;
    for (size_t n = 0; n < ir.size(); n++) {
        auto i = ir[n];
        if (i.kind == 3 || i.kind == 4)
            continue;
        if (i.kind == 8) {
            // Capture both operands before either insert: AL/AH and other
            // partial-register aliases can share the same cached host GPR.
            extract(11, 20 + i.dst, i.w, i.dstShift);
            extract(12, 20 + i.src, i.w, i.srcShift);
            insert(20 + i.dst, 12, i.w, i.dstShift);
            insert(20 + i.src, 11, i.w, i.srcShift);
            dirtyRegisters |= (1u << i.dst) | (1u << i.src);
            continue;
        }
        if (i.kind == 7) {
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
        if (i.kind == 6) {
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
        if (i.kind == 5) {
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
        if (i.kind == 1 && i.op >= 8)
            finishFlags(last, -1, 0);
        if (i.kind == 1 || i.kind == 2)
            last = n;
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
        if (i.kind == 0)
            mov(10, 12);
        else {
            uint32_t op = i.op == 0 || i.op == 8 ? 0x0b000000
                          : i.op == 1 ? 0x2a000000
                          : i.op == 4 ? 0x0a000000
                          : i.op == 6 ? 0x4a000000
                                      : 0x4b000000;
            emit(op | (12 << 16) | (11 << 5) | 10);
        }
        if (i.kind == 2 || (i.kind == 1 && i.op == 7))
            continue;
        dirtyRegisters |= 1u << i.dst;
        insert(dst, 10, i.w, i.dstShift);
    }
    flushRegisters(dirtyRegisters);
    bool branch = ir.back().kind == 4;
    if (!ir.back().nearReturn && ir.back().kind != 7) {
        imm(9, ir.back().nearCall ? ir.back().target
                                 : branch && ir.back().dst < 0 ? ir.back().imm : pos);
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
#endif
    return block;
}
} // namespace pc
