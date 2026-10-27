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
    const auto &terminal = ir.back();
    // A short decoder fallthrough ends at an interpreted instruction, so it
    // cannot acquire a native successor without invalidation. Spend link code
    // only on actual control-flow exits or a full-length decoded fragment.
    const bool linkCandidate = directLinks && !block.changesShadow && !block.pollBoundary && !terminal.nearReturn &&
        !terminal.indirectJump && !terminal.indirectCall && terminal.kind != Kind::IndirectJump &&
        (terminal.kind == Kind::Branch || terminal.kind == Kind::CounterBranch ||
         terminal.nearCall || ir.size() == 64);
    const bool linkNeedsFetchCheck = linkCandidate && (block.cr0 & 0x80000000u) &&
        std::any_of(ir.begin(), ir.end(), [](const auto &i) {
            return i.kind == Kind::Memory || i.nearCall || i.indirectCall || i.memoryPush;
        });
    size_t memoryMoves = 0;
    for (const auto &i : ir)
        if (i.kind == Kind::Memory)
            memoryMoves += 1 + bool(i.memoryCall || i.memoryPush);
    const size_t stringMoves=std::count_if(ir.begin(),ir.end(),[](const auto &i){return i.kind==Kind::String;});
    if (decoded.fetchTail.present || linkCandidate || memoryMoves || stringMoves || terminal.kind==Kind::FarTransfer || terminal.kind==Kind::FarReturn || terminal.kind==Kind::PortIO)
        block.native = std::make_unique<Block::NativeState>();
    if(decoded.fetchTail.present || terminal.kind==Kind::PortIO || terminal.kind==Kind::FarTransfer || terminal.kind==Kind::FarReturn) {
        block.native->boundary=std::make_unique<Block::BoundaryEntry>();
        block.native->boundary->fetchTail=decoded.fetchTail;
        if(terminal.kind==Kind::PortIO)block.native->boundary->operation=decoded.portIO;
        else if(terminal.kind==Kind::FarReturn)
            std::construct_at(&block.native->boundary->farReturn,NativeFarReturn{unsigned(terminal.w),uint16_t(terminal.imm),terminal.op==0xcf});
        else if(terminal.kind==Kind::FarTransfer) {
            block.native->boundary->farTransfer=std::make_unique<NativeFarTransfer>();
            auto &op=*block.native->boundary->farTransfer;
            op.memory=terminal.memory;op.width=terminal.w;op.offset=terminal.imm;
            op.selector=terminal.src;op.end=terminal.end;op.call=terminal.op;op.indirect=!terminal.immediate;
        }
    }
    if (linkCandidate) {
        block.native->link = std::make_unique<LinkState>();
        block.link()->startIP = cpu.ip;
        block.link()->generationData = cpu.generation.data();
    }
    auto pos = decoded.end;
    std::vector<uint32_t> code;
    std::vector<DebugRegion> debugRegions;
    auto mark = [&](const char *kind, uint32_t ip) {
        if (debugState) debugRegions.push_back({code.size()*4, kind, ip});
    };
    mark("Prologue", cpu.ip);
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
        if (i.memoryDoubleShift) usedRegisters |= 1u << i.src;
        if (i.memoryPop) usedRegisters |= 1u << SP;
        if (i.stackMany) usedRegisters |= 255;
        if (i.leaveFrame) usedRegisters |= 1u << SP;
        if (i.kind == Kind::Divide || i.memoryDivide || i.kind == Kind::MultiplyFull || i.memoryMultiplyFull)
            usedRegisters |= (1u << AX) | (i.w == 1 ? 0 : 1u << DX);
        if (i.indirectCall && !i.memoryCall) usedRegisters |= 1u << i.src;
        if (i.memoryCall || i.memoryPush) {
            if (i.callPointer.base >= 0) usedRegisters |= 1u << i.callPointer.base;
            if (i.callPointer.index >= 0) usedRegisters |= 1u << i.callPointer.index;
        }
        if ((i.kind == Kind::DoubleShift || i.memoryDoubleShift) && !i.immediate)
            usedRegisters |= 1u << CX;
        if (i.kind == Kind::Nop || i.kind == Kind::Branch || i.kind == Kind::Direction || i.kind == Kind::CarryControl || i.kind == Kind::InterruptControl || i.kind == Kind::String || i.kind == Kind::FarTransfer || i.kind == Kind::FarReturn || i.kind == Kind::PortIO)
            continue;
        if (!(i.kind == Kind::Memory && ((i.immediate && !i.memoryMultiply) || i.nearReturn || i.segmentRegister >= 0 || i.pushFlags || i.popFlags || i.memoryPop || i.memoryDoubleShift)))
            usedRegisters |= 1u << i.dst;
        if (i.kind == Kind::Memory || i.kind == Kind::Lea) {
            if (i.memory.base >= 0)
                usedRegisters |= 1u << i.memory.base;
            if (i.memory.index >= 0)
                usedRegisters |= 1u << i.memory.index;
        } else if (!i.immediate || i.kind == Kind::Multiply || i.kind == Kind::DoubleShift)
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
    int epochRegister = -1;
    auto savePair = [&](int pair) {
        return pair == 0 || (usedRegisters & (3u << (pair * 2 - 1))) || (pair == 4 && fastMemory) ||
               bitmapRegister == 19 + pair * 2 || bitmapRegister == 20 + pair * 2 ||
               epochRegister == 19 + pair * 2 || epochRegister == 20 + pair * 2;
    };
    if (fastMemory && (block.cr0 & 0x80000000u))
        for (unsigned pass = 0; pass < 2 && epochRegister < 0; ++pass)
            for (int reg = 7; reg >= 0; --reg) {
                int host = 20 + reg;
                if (!(usedRegisters & (1u << reg)) && host != bitmapRegister &&
                    (pass || savePair((host - 19) / 2))) {
                    epochRegister = host; // Prefer an already-saved register pair.
                    break;
                }
            }
    emit(0xa9b97bfdu); // stp x29,x30,[sp,#-128/-112]!
    emit(0x910003fdu); // mov x29,sp: keep a valid host frame chain for diagnostics.
    for (int k = 0; k < 5; k++)
        if (savePair(k))
            emit(0xa9000000u | (((16 + k * 16) / 8) << 15) | ((20 + k * 2) << 10) | (31 << 5) |
                 (19 + k * 2));
    emit(0xaa0003f3u); // mov x19,x0
    if (linkCandidate) {
        // Separate chain control from the ordinary emitter's operand, carry
        // and indirect-call spills at offsets 96..108. Linked entries reuse
        // this frame; matching save masks make every target epilogue valid.
        block.link()->entry = code.size()*4;
        for (int pair = 0; pair < 5; ++pair)
            if (savePair(pair)) block.link()->saveMask |= 1u << pair;
    }
    for (int reg = 0; reg < 8; reg++)
        if (usedRegisters & (1u << reg))
            load(20 + reg, 19, offsetof(CPU, r) + reg * 4);
    if (linkCandidate) {
        block.link()->loadedEntry = code.size()*4;
        block.link()->usedRegisters = usedRegisters;
    }
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
    if (epochRegister >= 0) {
        // Fetch validation synchronizes CR3/TLB before entry. Supported native
        // instructions and continuing RAM helpers cannot change the epoch;
        // MMIO, faults and state-changing instructions leave this block.
        cpuAddress(9, offsetof(CPU, tlbEpoch) & ~4095u);
        load(epochRegister, 9, offsetof(CPU, tlbEpoch) & 4095u);
    }
    auto call = [&](uintptr_t address) {
        pointer(16, address);
        emit(0xd63f0200u);
        if (linkNeedsFetchCheck && address == reinterpret_cast<uintptr_t>(&nativeMemory)) {
            block.link()->helperFlagSites.push_back(code.size()*4);
            emit(0xd503201fu); // Promotion stores the helper status without clobbering registers.
        }
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
    // x13 contains the full signed product; all non-CF/OF flags stay intact.
    auto multiplyFlags = [&](unsigned width) {
        emit(0x93400000u | ((width * 8 - 1) << 10) | (13 << 5) | 14);
        emit(0xeb00001fu | (14 << 16) | (13 << 5)); // cmp full,sign-extended low
        emit(0x1a9f07ebu); // cset w11,ne: signed overflow sets both CF and OF
        load(12, 19, offsetof(CPU, flags));
        imm(14, ~(CF | OF));
        emit(0x0a000000u | (14 << 16) | (12 << 5) | 12);
        for (unsigned bit : {0u, 11u})
            emit(0x33000000u | (((32 - bit) & 31) << 16) | (11 << 5) | 12);
        store(12, 19, offsetof(CPU, flags));
    };
    auto finishFlags = [&](int last, int condition, uint32_t target) {
        if (last >= 0 && ir[last].memoryMultiply) {
            emit(0xf9400000u | (12u << 10) | (31 << 5) | 13); // ldr x13,[sp,#96]
            multiplyFlags(ir[last].w);
            last = -1;
        }
        if (last < 0 && condition < 0)
            return;
        // Materialize supported ALU flags without a host helper. Match the
        // interpreter's deterministic undefined-AF value for logical ops too.
        bool logical = last >= 0 &&
                       (ir[last].op == 1 || ir[last].op == 4 || ir[last].op == 6);
        bool arithmetic = last >= 0 &&
                          (ir[last].op == 0 || ir[last].op == 2 || ir[last].op == 3 ||
                           ir[last].op == 5 || ir[last].op == 7 ||
                           ir[last].op == 8 || ir[last].op == 9);
        if (logical || arithmetic) {
            load(9, 31, 96);
            load(10, 31, 100);
            auto op = ir[last].op;
            bool addition = op == 0 || op == 2 || op == 8, preserveCarry = op >= 8;
            bool withCarry = op == 2 || op == 3;
            if (arithmetic) {
                extract(9, 9, ir[last].w, 0);
                extract(10, 10, ir[last].w, 0);
                // Zero-extended inputs allow a full 33-bit sum for 32-bit CF.
                emit((addition ? 0x8b000000u : withCarry ? 0xcb000000u : 0x4b000000u) |
                     (10 << 16) | (9 << 5) | 11);
                if (withCarry) {
                    load(14, 31, 104);
                    emit((addition ? 0x8b000000u : 0xcb000000u) |
                         (14 << 16) | (11 << 5) | 11);
                }
            } else
                emit((op == 1 ? 0x2a000000u : op == 4 ? 0x0a000000u : 0x4a000000u) |
                     (10 << 16) | (9 << 5) | 11);
            load(12, 19, offsetof(CPU, flags));
            imm(14, ~((preserveCarry ? 0 : CF) | PF | AF | ZF | SF | OF));
            emit(0x0a000000u | (14 << 16) | (12 << 5) | 12);
            imm(14, 2);
            emit(0x2a000000u | (14 << 16) | (12 << 5) | 12);
            if (arithmetic && !preserveCarry) {
                if (addition || withCarry) {
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
    if (memoryMoves) block.native->memory.reserve(memoryMoves);
    for (auto i : ir)
        if (i.kind == Kind::Memory) {
            if (i.memoryCall || i.memoryPush) block.native->memory.push_back(i.callPointer);
            block.native->memory.push_back(i.memory);
        }
    if(stringMoves) {
        block.native->strings.reserve(stringMoves);
        for(const auto &i:ir)if(i.kind==Kind::String)block.native->strings.push_back(i.string);
    }
    size_t stringIndex=0;
    size_t memoryIndex = 0;
    unsigned dirtyRegisters = 0;
    auto flushRegisters = [&](unsigned dirty) {
        for (int reg = 0; reg < 8; reg++)
            if (dirty & (1u << reg))
                store(20 + reg, 19, offsetof(CPU, r) + reg * 4);
    };
    std::vector<size_t> exits;
    int last = -1;
    if (linkCandidate) block.link()->loopEntry = code.size()*4;
    if (linkNeedsFetchCheck) {
        block.link()->resetFlagOffset = code.size()*4;
        emit(0xd503201fu); // Remains a NOP until a linked frame exists.
    }
    auto selector = [&](int dst, int segment) {
        const unsigned offset = offsetof(CPU,s) + segment*sizeof(Segment) + offsetof(Segment,selector);
        emit(0x79400000u | ((offset/2) << 10) | (19 << 5) | dst);
    };
    // Signed operands arrive in w11/w12. Register and memory forms share
    // overflow detection and exact preservation of the other guest flags.
    auto multiplyResult = [&](const jit_detail::Instruction &i, bool flags, bool defer = false) {
        emit(0x9b207c00u | (12 << 16) | (11 << 5) | 13); // smull x13,w11,w12
        if (defer) {
            // A later full flag writer can discard this product. Fault/code
            // exits materialize its CF/OF from the saved complete 64-bit value.
            emit(0xf9000000u | (12u << 10) | (31 << 5) | 13); // str x13,[sp,#96]
        } else if (flags) multiplyFlags(i.w);
        insert(20 + i.dst, 13, i.w, 0);
        dirtyRegisters |= 1u << i.dst;
    };
    // The right operand is captured in w12 before either implicit result GPR
    // changes. Full 64-bit products cover every byte/word/dword input pair.
    auto multiplyFullResult = [&](const jit_detail::Instruction &i,bool flags) {
        extract(11,20+AX,i.w,0);
        if(i.op==5) {
            emit(0x13000000u | ((i.w*8-1)<<10) | (11<<5) | 11);
            emit(0x13000000u | ((i.w*8-1)<<10) | (12<<5) | 12);
        }
        emit((i.op==5?0x9b207c00u:0x9ba07c00u) | (12<<16) | (11<<5) | 13); // smull/umull
        if(flags) {
            if(i.op==5)multiplyFlags(i.w);
            else {
                emit(0xd3400000u | ((i.w*8)<<16) | (63<<10) | (13<<5) | 14); // lsr x14,x13,#bits
                emit(0xf100001fu | (14<<5)); // cmp x14,#0
                emit(0x1a9f07ebu); // cset w11,ne
                load(12,19,offsetof(CPU,flags));
                imm(14,~(CF|OF));
                emit(0x0a000000u | (14<<16) | (12<<5) | 12);
                for(unsigned bit:{0u,11u})
                    emit(0x33000000u | (((32-bit)&31)<<16) | (11<<5) | 12);
                store(12,19,offsetof(CPU,flags));
            }
        }
        insert(20+AX,13,i.w==1?2:i.w,0);
        dirtyRegisters |= 1u<<AX;
        if(i.w!=1) {
            emit(0xd3400000u | ((i.w*8)<<16) | (63<<10) | (13<<5) | 14);
            insert(20+DX,14,i.w,0);
            dirtyRegisters |= 1u<<DX;
        }
    };
    auto replay = [&](uint32_t start, unsigned completed, unsigned dirty) {
        flushRegisters(dirty);
        finishFlags(last, -1, 0);
        static_assert(offsetof(CPU, nativeFallback) < 4096);
        imm(9, 1);
        emit(0x39000000u | (offsetof(CPU, nativeFallback) << 10) | (19 << 5) | 9);
        imm(9, start); store(9, 19, offsetof(CPU, ip));
        imm(0, completed);
        exits.push_back(code.size()); emit(0x14000000u);
    };
    // w10 holds the selector. Failure is side-effect-free and replays only
    // after returning from the generated frame with its prior state committed.
    auto segmentLoad = [&](const jit_detail::Instruction &i, unsigned n, unsigned dirty) {
        mov(2,10);
        emit(0xaa1303e0u);
        imm(1,i.segmentRegister);
        imm(3,n+1);
        call(reinterpret_cast<uintptr_t>(&jit_detail::nativeSegment));
        const auto success=code.size();emit(0x35000000u); // cbnz w0,success
        replay(i.start,n,dirty);
        code[success] |= uint32_t(code.size()-success) << 5;
    };
    // Divisor arrives in w10. Checks precede every implicit GPR update; failed
    // divisions replay in C++ after the native frame returns, preserving the
    // interpreter's existing error behavior without unwinding through ARM64.
    auto divideResult = [&](const jit_detail::Instruction &i, std::vector<size_t> &errors) {
        auto reject = [&](uint32_t branch) { errors.push_back(code.size()); emit(branch); };
        reject(0x3400000au); // cbz w10: divisor zero
        if (i.op == 7)
            emit(0x93400000u | ((i.w * 8 - 1) << 10) | (10 << 5) | 10);
        extract(11, 20 + AX, i.w == 1 ? 2 : i.w, 0);
        if (i.w != 1) {
            extract(12, 20 + DX, i.w, 0);
            emit(0xaa000000u | (12 << 16) | ((i.w * 8) << 10) | (11 << 5) | 11);
        }
        if (i.op == 7 && i.w < 4)
            emit(0x93400000u | ((i.w * 16 - 1) << 10) | (11 << 5) | 11);
        if (i.op == 7 && i.w == 4) {
            pointer(13, uint64_t(1) << 63);
            emit(0xeb00001fu | (13 << 16) | (11 << 5));
            const auto notMinimum = code.size(); emit(0x54000001u); // b.ne safe
            emit(0xb100001fu | (1u << 10) | (10 << 5)); // cmn x10,#1: denominator -1
            reject(0x54000000u); // b.eq: INT64_MIN / -1
            code[notMinimum] |= uint32_t(code.size() - notMinimum) << 5;
        }
        emit((i.op == 7 ? 0x9ac00c00u : 0x9ac00800u) | (10 << 16) | (11 << 5) | 13);
        if (i.op == 7) {
            emit(0x93400000u | ((i.w * 8 - 1) << 10) | (13 << 5) | 14);
            emit(0xeb00001fu | (14 << 16) | (13 << 5));
            reject(0x54000001u); // quotient differs from its signed truncation
        } else {
            emit(0xd3400000u | ((i.w * 8) << 16) | (63u << 10) | (13 << 5) | 14);
            reject(0xb500000eu); // quotient exceeds unsigned operand width
        }
        emit(0x9b008000u | (10 << 16) | (11 << 10) | (13 << 5) | 14); // remainder = num - q*den
        insert(20 + AX, 13, i.w, 0);
        insert(20 + (i.w == 1 ? AX : DX), 14, i.w, i.w == 1 ? 8 : 0);
        dirtyRegisters |= (1u << AX) | (i.w == 1 ? 0 : 1u << DX);
    };
    auto commitShift = [&](const jit_detail::Instruction &i, unsigned value, bool memory) {
        if (memory) {
            emit(0xf9400000u | (12u << 10) | (31 << 5) | 15); // ldr x15,[sp,#96]: guarded RAM address
            emit((i.w == 4 ? 0xb9000000u : i.w == 2 ? 0x79000000u : 0x39000000u) | (15 << 5) | value);
        } else insert(20 + i.dst, value, i.w, i.dstShift);
    };
    // Registers and guarded memory share all count, result and flag semantics.
    // Memory input is captured in w17 after checks; its host address is at sp+96.
    auto shiftResult = [&](const jit_detail::Instruction &i, bool flags, bool memory = false) {
        if (i.immediate && !i.imm) return;
        if (i.kind == Kind::Rotate) {
            finishFlags(last, -1, 0);
            last = -1;
            const unsigned width = i.w * 8;
            const bool carry = i.op >= 2, left = !(i.op & 1);
            const unsigned ring = width + unsigned(carry);
            if (i.immediate) imm(9, i.imm);
            else {
                bits(9, 20 + CX, 0, 5);
                if (!carry && width < 32) bits(9, 9, 0, i.w == 1 ? 3 : 4);
                else if (carry && width < 32) {
                    imm(10, ring);
                    emit(0x1ac00800u | (10 << 16) | (9 << 5) | 14); // udiv quotient,count,ring
                    emit(0x1b008000u | (10 << 16) | (9 << 10) | (14 << 5) | 9); // msub count,quotient,ring,count
                }
            }
            const auto zero = code.size();
            if (!i.immediate) emit(0x34000009u); // cbz normalized count: preserve GPRs and every flag
            extract(11, memory ? 17 : 20 + i.dst, i.w, memory ? 0 : i.dstShift);
            load(12, 19, offsetof(CPU, flags));
            if (carry) {
                // Put CF below the operand to form a 9/17/33-bit ring.
                emit(0xd3400000u | (63u << 16) | (62u << 10) | (11 << 5) | 11); // lsl x11,#1
                bits(14, 12, 0, 1);
                emit(0xaa000000u | (14 << 16) | (11 << 5) | 11);
            }
            imm(10, ring);
            emit(0x4b000000u | (9 << 16) | (10 << 5) | 10); // complementary count
            emit((left ? 0x9ac02000u : 0x9ac02400u) | (9 << 16) | (11 << 5) | 13);
            emit((left ? 0x9ac02400u : 0x9ac02000u) | (10 << 16) | (11 << 5) | 14);
            emit(0xaa000000u | (14 << 16) | (13 << 5) | 13);
            emit(0xd3400000u | ((ring - 1) << 10) | (13 << 5) | 13); // truncate ring, including bit 32
            bits(14, 13, !carry && !left ? width - 1 : 0, 1);
            flagBit(14, 0);
            if (carry)
                emit(0xd3400000u | (1u << 16) | (63u << 10) | (13 << 5) | 11); // operand = ring >> 1
            else mov(11, 13);
            size_t preserveOverflow = size_t(-1);
            if (!i.immediate) {
                emit(0x7100053fu); // cmp w9,#1
                preserveOverflow = code.size(); emit(0x54000001u); // b.ne preserve OF
            }
            if (!i.immediate || i.imm == 1) {
                bits(14, 11, width - 1, 1);
                if (left) bits(15, 12, 0, 1);
                else bits(15, 11, width - 2, 1);
                emit(0x4a000000u | (15 << 16) | (14 << 5) | 14);
                flagBit(14, 11);
            }
            if (preserveOverflow != size_t(-1))
                code[preserveOverflow] |= uint32_t(code.size() - preserveOverflow) << 5;
            store(12, 19, offsetof(CPU, flags));
            commitShift(i, 11, memory);
            if (!memory) dirtyRegisters |= 1u << i.dst;
            if (!i.immediate) code[zero] |= uint32_t(code.size() - zero) << 5;
            return;
        }
        if (i.kind == Kind::Shift && !i.immediate) {
            if (!memory && i.w == 4 && !flags) {
                // ARM variable shifts already mask their count to five bits,
                // including when source, count and destination all alias CX.
                unsigned dst = 20 + i.dst;
                auto op = i.op == 4 || i.op == 6 ? 0x1ac02000u
                          : i.op == 7 ? 0x1ac02800u : 0x1ac02400u;
                emit(op | ((20 + CX) << 16) | (dst << 5) | dst);
                if (!memory) dirtyRegisters |= 1u << i.dst;
                return;
            }
            if (flags) {
                finishFlags(last, -1, 0);
                last = -1;
            }
            unsigned width = i.w * 8;
            bool left = i.op == 4 || i.op == 6, arithmetic = i.op == 7;
            bits(9, 20 + CX, 0, 5); // Capture CL before an aliased destination changes.
            auto zero = code.size(); emit(0x34000009u);
            extract(11, memory ? 17 : 20 + i.dst, i.w, memory ? 0 : i.dstShift);
            if (flags) {
                emit(0x51000400u | (9 << 5) | 10); // count - 1
                if (arithmetic) {
                    emit(0x13000000u | ((width - 1) << 10) | (11 << 5) | 14);
                    emit(0x1ac02800u | (10 << 16) | (14 << 5) | 14); // asrv: last bit out
                } else {
                    emit((left ? 0x1ac02000u : 0x1ac02400u) | (10 << 16) | (11 << 5) | 14);
                    if (left) bits(14, 14, width - 1, 1);
                }
            }
            if (arithmetic) {
                emit(0x13000000u | ((width - 1) << 10) | (11 << 5) | 13);
                emit(0x1ac02800u | (9 << 16) | (13 << 5) | 13);
            } else {
                emit((left ? 0x1ac02000u : 0x1ac02400u) | (9 << 16) | (11 << 5) | 13);
            }
            extract(13, 13, i.w, 0);
            if (flags) {
                load(12, 19, offsetof(CPU, flags));
                imm(10, ~(CF | SF | ZF | PF)); // AF and undefined multi-bit OF are preserved.
                emit(0x0a000000u | (10 << 16) | (12 << 5) | 12);
                flagBit(14, 0);
                emit(0x7100053fu); // cmp w9,#1
                auto many = code.size(); emit(0x54000001u);
                if (arithmetic) imm(14, 0);
                else {
                    bits(10, left ? 13 : 11, width - 1, 1);
                    if (left) emit(0x4a000000u | (10 << 16) | (14 << 5) | 14);
                    else mov(14, 10);
                }
                flagBit(14, 11);
                code[many] |= uint32_t(code.size() - many) << 5;
                emit(0x710001bfu); // cmp w13,#0
                emit(0x1a9f17eeu); flagBit(14, 6);
                bits(14, 13, width - 1, 1); flagBit(14, 7);
                bits(14, 13, 0, 8);
                for (unsigned shift : {4u, 2u, 1u})
                    emit(0x4a400000u | (14 << 16) | (shift << 10) | (14 << 5) | 14);
                emit(0x2a2e03eeu); flagBit(14, 2);
                store(12, 19, offsetof(CPU, flags));
            }
            commitShift(i, 13, memory);
            code[zero] |= uint32_t(code.size() - zero) << 5;
            if (!memory) dirtyRegisters |= 1u << i.dst;
            return;
        }
        if (i.kind == Kind::Shift) {
            if (flags) {
                finishFlags(last, -1, 0);
                last = -1;
            }
            unsigned width = i.w * 8, count = i.imm;
            bool left = i.op == 4 || i.op == 6, arithmetic = i.op == 7;
            extract(9, memory ? 17 : 20 + i.dst, i.w, memory ? 0 : i.dstShift);
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
            if (flags) {
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
            commitShift(i, 11, memory);
            if (!memory) dirtyRegisters |= 1u << i.dst;
            return;
        }
    };
    auto doubleShiftResult = [&](const jit_detail::Instruction &i, bool memory) {
        if (i.immediate) imm(9, i.imm);
        else bits(9, 20 + CX, 0, 5);
        auto zero = code.size();
        emit(0x34000009u); // cbz w9: count zero preserves destination and flags
        extract(11, memory ? 17 : 20 + i.dst, i.w, 0);
        extract(12, 20 + i.src, i.w, 0);
        if (i.op) {
            emit(0xaa000000u | (11 << 16) | ((i.w * 8) << 10) | (12 << 5) | 13);
            imm(10,i.w*16);
            emit(0x4b000000u | (9 << 16) | (10 << 5) | 10); // pair width - count
            emit(0x9ac02400u | (10 << 16) | (13 << 5) | 14); // carry from original pair
            emit(0x9ac02000u | (9 << 16) | (13 << 5) | 13); // lslv x13,pair,count
            emit(0xd340fc00u | ((i.w*8) << 16) | (13 << 5) | 13); // lsr x13,#operand bits
        } else {
            emit(0xaa000000u | (12 << 16) | ((i.w * 8) << 10) | (11 << 5) | 13);
            emit(0x51000400u | (9 << 5) | 10); // count - 1
            emit(0x9ac02400u | (10 << 16) | (13 << 5) | 14);
            emit(0x9ac02400u | (9 << 16) | (13 << 5) | 13);
        }
        extract(13, 13, i.w, 0);
        load(12, 19, offsetof(CPU, flags));
        imm(10, ~(CF | PF | AF | ZF | SF | OF));
        emit(0x0a000000u | (10 << 16) | (12 << 5) | 12);
        imm(10, AF | 2); // Reference AND(result,mask) defines undefined AF as 1.
        emit(0x2a000000u | (10 << 16) | (12 << 5) | 12);
        flagBit(14, 0);
        emit(0x7100053fu); // cmp w9,#1
        auto notOne = code.size(); emit(0x54000001u); // b.ne
        emit(0x4a000000u | (13 << 16) | (11 << 5) | 14);
        bits(14, 14, i.w * 8 - 1, 1); flagBit(14, 11);
        code[notOne] |= uint32_t(code.size() - notOne) << 5;
        emit(0x710001bfu); // cmp w13,#0
        emit(0x1a9f17eeu); // cset w14,eq
        flagBit(14, 6);
        bits(14, 13, i.w * 8 - 1, 1); flagBit(14, 7);
        bits(14, 13, 0, 8);
        for (unsigned shift : {4u, 2u, 1u})
            emit(0x4a400000u | (14 << 16) | (shift << 10) | (14 << 5) | 14);
        emit(0x2a2e03eeu); flagBit(14, 2);
        store(12, 19, offsetof(CPU, flags));
        if (memory) {
            emit(0xf9400000u | (12u << 10) | (31 << 5) | 15); // Guarded RAM address.
            emit((i.w==4 ? 0xb9000000u : 0x79000000u) | (15 << 5) | 13);
        } else insert(20 + i.dst, 13, i.w, 0);
        code[zero] |= uint32_t(code.size() - zero) << 5;
        if (!memory) dirtyRegisters |= 1u << i.dst;
    };
    for (size_t n = 0; n < ir.size(); n++) {
        auto i = ir[n];
        static constexpr const char *names[] = {"Move", "Alu", "Test", "Nop", "Branch",
            "Memory", "Lea", "CounterBranch", "Exchange", "Multiply", "Shift", "IndirectJump",
            "MultiplyFull", "DoubleShift", "BitTest", "LoadFlags", "Direction", "Rotate", "SignExtend", "Invert", "Divide", "SegmentRead", "SegmentLoad", "InterruptControl", "String", "StoreFlags", "CarryControl", "SetCondition", "PortIO", "FarReturn", "FarTransfer"};
        mark(names[static_cast<unsigned>(i.kind)], i.start);
        if (i.kind == Kind::Nop)
            continue;
        if(i.kind==Kind::FarTransfer || i.kind==Kind::FarReturn) {
            flushRegisters(dirtyRegisters);
            finishFlags(last,-1,0);
            dirtyRegisters=0;last=-1;
            imm(9,i.start);store(9,19,offsetof(CPU,ip));
            emit(0xaa1303e0u);
            if(i.kind==Kind::FarReturn) {
                pointer(1,reinterpret_cast<uintptr_t>(&block.native->boundary->farReturn));
                call(reinterpret_cast<uintptr_t>(&jit_detail::nativeFarReturn));
            } else {
                pointer(1,reinterpret_cast<uintptr_t>(block.native->boundary->farTransfer.get()));
                call(reinterpret_cast<uintptr_t>(&jit_detail::nativeFarTransfer));
            }
            const auto success=code.size();emit(0x35000000u);
            replay(i.start,unsigned(n),0);
            code[success] |= uint32_t(code.size()-success)<<5;
            continue;
        }
        if(i.kind==Kind::PortIO) {
            // Device callbacks execute in the C++ dispatcher after this frame
            // returns. Commit the exact prefix and retain the I/O restart IP.
            flushRegisters(dirtyRegisters);
            finishFlags(last,-1,0);
            dirtyRegisters=0;last=-1;
            imm(9,i.start);store(9,19,offsetof(CPU,ip));
            continue;
        }
        if(i.kind==Kind::String) {
            // The terminal helper consumes canonical CPU GPRs/flags and commits
            // its dynamic IP and repeat continuation. No stale cached GPR may
            // overwrite its result in the ordinary epilogue.
            flushRegisters(dirtyRegisters);
            finishFlags(last,-1,0);
            last=-1;dirtyRegisters=0;
            imm(9,i.start);store(9,19,offsetof(CPU,ip));
            emit(0xaa1303e0u);
            pointer(1,reinterpret_cast<uintptr_t>(&block.native->strings[stringIndex++]));
            call(reinterpret_cast<uintptr_t>(&jit_detail::nativeString));
            const auto success=code.size();emit(0x35000000u);
            replay(i.start,unsigned(n),0);
            code[success] |= uint32_t(code.size()-success) << 5;
            continue;
        }
        if (i.kind == Kind::InterruptControl) {
            load(12,19,offsetof(CPU,flags));
            const unsigned cpl = block.vm ? 3 : (block.cr0 & 1) ? cpu.s[CS].selector & 3 : 0;
            if (cpl) {
                bits(13,12,12,2);
                emit(0x7100001fu | (cpl << 10) | (13 << 5));
                const auto allowed=code.size();emit(0x54000002u); // b.hs: IOPL >= CPL
                replay(i.start,unsigned(n),dirtyRegisters);
                code[allowed] |= uint32_t(code.size()-allowed) << 5;
            }
            imm(13,IF);
            emit((i.op ? 0x2a000000u : 0x0a200000u) | (13 << 16) | (12 << 5) | 12);
            store(12,19,offsetof(CPU,flags));
            if (i.op) {
                // STI blocks never run inside an unaccounted cached/linked tail.
                // Count the prefix and this instruction from the frame's base.
                static_assert(offsetof(CPU,instructions)%8 == 0);
                static_assert(offsetof(CPU,interruptBlockedThrough)%8 == 0);
                emit(0xf9400000u | ((offsetof(CPU,instructions)/8) << 10) | (19 << 5) | 9);
                emit(0x91000000u | ((n+1) << 10) | (9 << 5) | 9);
                emit(0xf9000000u | ((offsetof(CPU,interruptBlockedThrough)/8) << 10) | (19 << 5) | 9);
            }
            continue;
        }
        if (i.kind == Kind::SegmentRead) {
            selector(10,i.segmentRegister);
            insert(20+i.dst,10,i.w,0);
            dirtyRegisters |= 1u << i.dst;
            continue;
        }
        if (i.kind == Kind::SegmentLoad) {
            extract(10,20+i.dst,2,0);
            segmentLoad(i,unsigned(n),dirtyRegisters);
            continue;
        }
        if (i.kind == Kind::IndirectJump) {
            // Operand size selects the target width independently of CS.big.
            extract(10, 20 + i.src, i.w, 0);
            store(10, 19, offsetof(CPU, ip));
            continue;
        }
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
        if (i.kind == Kind::Direction) {
            load(9, 19, offsetof(CPU, flags));
            imm(10, DF);
            emit((i.imm ? 0x2a000000u : 0x0a200000u) | (10 << 16) | (9 << 5) | 9);
            store(9, 19, offsetof(CPU, flags));
            continue;
        }
        if(i.setCondition>=0) {
            finishFlags(last,-1,0);
            last=-1;
            conditionValue(i.setCondition,true);
            if(i.setCondition&1) {
                imm(14,1);
                emit(0x4a000000u | (14<<16) | (13<<5) | 13);
            }
            if(i.kind==Kind::SetCondition) {
                insert(20+i.dst,13,1,i.dstShift);
                dirtyRegisters |= 1u<<i.dst;
                continue;
            }
            // Address/translation guards and the slow helper may clobber the
            // predicate temporaries. Retain the captured byte until the store.
            store(13,31,104);
        }
        if (i.kind == Kind::CarryControl || i.kind == Kind::StoreFlags) {
            finishFlags(last,-1,0);
            last=-1;
            load(12,19,offsetof(CPU,flags));
            if(i.kind==Kind::StoreFlags) {
                // SAHF replaces only SF/ZF/AF/PF/CF; OF and all control bits
                // survive. AH's reserved bits cannot enter the flags image.
                imm(13,0xd5);
                emit(0x0a200000u | (13<<16) | (12<<5) | 12);
                extract(11,20+AX,1,8);
                emit(0x0a000000u | (13<<16) | (11<<5) | 11);
                emit(0x2a000000u | (11<<16) | (12<<5) | 12);
                imm(13,2);
                emit(0x2a000000u | (13<<16) | (12<<5) | 12); // Reserved bit 1 is always set.
            } else {
                imm(13,CF);
                const auto op=i.op==0xf5?0x4a000000u:i.op==0xf9?0x2a000000u:0x0a200000u;
                emit(op | (13<<16) | (12<<5) | 12);
            }
            store(12,19,offsetof(CPU,flags));
            continue;
        }
        if (i.kind == Kind::LoadFlags) {
            finishFlags(last, -1, 0);
            last = -1;
            load(11, 19, offsetof(CPU, flags));
            insert(20 + AX, 11, 1, 8);
            dirtyRegisters |= 1u << AX;
            continue;
        }
        if (i.kind == Kind::Divide) {
            const auto prefixDirty = dirtyRegisters;
            extract(10, 20 + i.src, i.w, i.srcShift);
            std::vector<size_t> errors;
            divideResult(i, errors);
            const auto success = code.size(); emit(0x14000000u);
            for (auto at : errors) code[at] |= uint32_t(code.size() - at) << 5;
            replay(i.start, unsigned(n), prefixDirty);
            code[success] |= uint32_t(code.size() - success);
            continue;
        }
        if (i.kind == Kind::Invert) {
            extract(11, 20 + i.dst, i.w, i.dstShift);
            emit(0x2a2003e0u | (11 << 16) | 11); // mvn w11,w11
            insert(20 + i.dst, 11, i.w, i.dstShift);
            dirtyRegisters |= 1u << i.dst;
            continue;
        }
        if (i.kind == Kind::SignExtend) {
            emit(0x13000000u | ((i.sourceWidth * 8 - 1) << 10) | ((20 + AX) << 5) | 11);
            if (i.op) emit(0x131f7c00u | (11 << 5) | 11); // asr w11,#31: CWD/CDQ sign fill
            insert(20 + i.dst, 11, i.w, 0);
            dirtyRegisters |= 1u << i.dst;
            continue;
        }
        if (i.kind == Kind::Rotate || i.kind == Kind::Shift) {
            shiftResult(i, neededFlags[n]);
            continue;
        }
        if (i.kind == Kind::BitTest) {
            if (neededFlags[n]) {
                finishFlags(last, -1, 0);
                last = -1;
            }
            // Capture the index before modifying an aliased destination.
            if (i.immediate) imm(9, i.imm);
            else bits(9, 20 + i.src, 0, i.w == 2 ? 4 : 5);
            extract(11, 20 + i.dst, i.w, 0);
            if (neededFlags[n]) {
                emit(0x1ac02400u | (9 << 16) | (11 << 5) | 13); // lsrv w13,w11,w9
                load(12, 19, offsetof(CPU, flags));
                flagBit(13, 0); // Preserve the reference's other flags verbatim.
                store(12, 19, offsetof(CPU, flags));
            }
            if (i.op != 4) {
                imm(10, 1);
                emit(0x1ac02000u | (9 << 16) | (10 << 5) | 10); // lslv w10,w10,w9
                auto op = i.op == 5 ? 0x2a000000u : i.op == 6 ? 0x0a200000u : 0x4a000000u;
                emit(op | (10 << 16) | (11 << 5) | 11); // orr / bic / eor
                insert(20 + i.dst, 11, i.w, 0);
                dirtyRegisters |= 1u << i.dst;
            }
            continue;
        }
        if (i.kind == Kind::DoubleShift) {
            finishFlags(last,-1,0);
            last=-1;
            doubleShiftResult(i,false);
            continue;
        }
        if (i.kind == Kind::MultiplyFull) {
            finishFlags(last,-1,0);
            last=-1;
            extract(12,20+i.src,i.w,i.srcShift);
            multiplyFullResult(i,neededFlags[n]);
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
            multiplyResult(i, neededFlags[n]);
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
            if (i.op <= 0xe1) {
                finishFlags(last,-1,0); // Conditional LOOP consumes prior ZF.
                last=-1;
            }
            extract(11, 20 + CX, i.w, 0);
            if (i.op != 0xe3) {
                emit(0x5100056bu); // sub w11,w11,#1, without changing NZCV
                insert(20 + CX, 11, i.w, 0);
                dirtyRegisters |= 1u << CX;
            }
            imm(9, pos);
            store(9, 19, offsetof(CPU, ip));
            auto fallthrough = code.size();
            emit(i.op != 0xe3 ? 0x3400000bu : 0x3500000bu); // cbz/cbnz w11
            size_t wrongZero=size_t(-1);
            if (i.op <= 0xe1) {
                load(12,19,offsetof(CPU,flags));
                bits(12,12,6,1);
                wrongZero=code.size();
                emit(i.op==0xe0 ? 0x3500000cu : 0x3400000cu); // LOOPNE/LOOPE reject the opposite ZF.
            }
            imm(9, i.imm);
            store(9, 19, offsetof(CPU, ip));
            code[fallthrough] |= uint32_t(code.size() - fallthrough) << 5;
            if(wrongZero!=size_t(-1))code[wrongZero] |= uint32_t(code.size()-wrongZero) << 5;
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
            std::vector<size_t> invalidCalls;
            if (i.memoryPop) {
                const auto &op=i.memory;
                // Reconstruct ESP after the pop, preserving its high half for
                // a 16-bit stack even when the destination uses 32-bit addressing.
                mov(9,20+SP);
                emit(0x11000000u | (i.w << 10) | ((20+SP) << 5) | 10);
                insert(9,10,block.stack32 ? 4 : 2,0);
                imm(3,op.displacement);
                if (op.base >= 0)
                    emit(0x0b000000u | ((op.base==SP ? 9 : 20+op.base) << 16) | (3 << 5) | 3);
                if (op.index >= 0)
                    emit(0x0b000000u | ((20+op.index) << 16) | (op.scale << 10) | (3 << 5) | 3);
                if (!op.address32) extract(3,3,2,0);
                extract(2,20+SP,block.stack32 ? 4 : 2,0);
                emit(0xaa1303e0u);
                pointer(1,reinterpret_cast<uintptr_t>(&block.native->memory[memoryIndex++]));
                call(reinterpret_cast<uintptr_t>(&jit_detail::nativePopMemory));
                const auto success=code.size();emit(0x35000000u); // cbnz w0,success
                replay(i.start,unsigned(n),dirtyRegisters);
                code[success] |= uint32_t(code.size()-success) << 5;
                emit(0x11000000u | (i.w << 10) | ((20+SP) << 5) | 11);
                insert(20+SP,11,block.stack32 ? 4 : 2,0);
                dirtyRegisters |= 1u << SP;
                continue;
            }
            if (i.nearCall || i.memoryPush) {
                if (i.memoryCall || i.memoryPush) {
                    // Read the source with the old SP/ESP and guarded RAM-only
                    // semantics. A later stack failure may replay this plain RAM
                    // read; no device callback or guest write has occurred.
                    const auto &op = i.callPointer;
                    imm(2, op.displacement);
                    if (op.base >= 0)
                        emit(0x0b000000u | ((20 + op.base) << 16) | (2 << 5) | 2);
                    if (op.index >= 0)
                        emit(0x0b000000u | ((20 + op.index) << 16) | (op.scale << 10) | (2 << 5) | 2);
                    if (!op.address32) extract(2, 2, 2, 0);
                    emit(0xaa1303e0u);
                    pointer(1, reinterpret_cast<uintptr_t>(&block.native->memory[memoryIndex++]));
                    imm(3, 0);
                    imm(4, block.page);
                    imm(5, block.generation);
                    call(reinterpret_cast<uintptr_t>(&nativeMemory));
                    invalidCalls.push_back(code.size()); emit(0x34000000u); // cbz w0,replay
                    load(9, 19, offsetof(CPU, nativeValue));
                    store(9, 31, 108);
                } else if (i.indirectCall) {
                    // Capture the target before the push changes SP/ESP.
                    extract(9, 20 + i.src, i.w, 0);
                    store(9, 31, 108);
                } else imm(9, i.target);
                if (i.nearCall) {
                    load(10, 19, offsetof(CPU, s) + CS * sizeof(Segment) + offsetof(Segment, limit));
                    emit(0x6b00001fu | (10 << 16) | (9 << 5));
                    invalidCalls.push_back(code.size()); emit(0x54000008u); // b.hi replay before stack write
                }
            }
            if (i.pushFlags || i.popFlags) {
                // PUSHF observes lazy arithmetic flags; POPF retains reserved
                // bits and must expose exact prior flags on access faults.
                finishFlags(last,-1,0);
                last = -1;
                if (block.vm) {
                    load(10,19,offsetof(CPU,flags));
                    bits(10,10,12,2);
                    emit(0x71000d5fu); // cmp w10,#3
                    const auto allowed=code.size();emit(0x54000000u);
                    replay(i.start,unsigned(n),dirtyRegisters);
                    code[allowed] |= uint32_t(code.size()-allowed) << 5;
                }
            }
            const bool memoryCarry = i.memoryAlu && (i.op == 2 || i.op == 3);
            if (i.memory.delta || memoryCarry || i.memory.bit >= 4 || i.memoryMultiplyFull || i.memoryMultiply || i.memoryShift || i.memoryDoubleShift) {
                // INC/DEC preserve CF; ADC/SBB consume it; bit tests preserve
                // all other flags; IMUL preserves its undefined flags too.
                // Publish the exact restart state before
                // access/replay and capture carry across helpers.
                finishFlags(last, -1, 0);
                last = -1;
                if (memoryCarry) {
                    load(13, 19, offsetof(CPU, flags));
                    bits(13, 13, 0, 1);
                    store(13, 31, 104);
                }
            }
            size_t zeroDoubleEnd=size_t(-1);
            if (i.memoryDoubleShift && (!i.immediate || !i.imm)) {
                size_t nonzero=size_t(-1);
                if (!i.immediate) {
                    bits(9,20+CX,0,5);
                    nonzero=code.size();emit(0x35000009u); // cbnz w9,normal memory path
                }
                if ((block.cr0 & 1) && !block.vm) {
                    const auto offset=offsetof(CPU,s)+i.memory.segment*sizeof(Segment)+offsetof(Segment,unusable);
                    emit(0x39400000u | (offset << 10) | (19 << 5) | 9);
                    const auto usable=code.size();emit(0x34000009u);
                    replay(i.start,unsigned(n),dirtyRegisters);
                    code[usable] |= uint32_t(code.size()-usable) << 5;
                }
                zeroDoubleEnd=code.size();emit(0x14000000u);
                if (nonzero!=size_t(-1))code[nonzero] |= uint32_t(code.size()-nonzero) << 5;
            }
            const auto accessWidth = i.memory.width;
            auto beforeLoad = dirtyRegisters;
            auto metadata = &block.native->memory[memoryIndex++];
            std::vector<size_t> divisionErrors;
            auto finishMove = [&]() {
                if (i.memoryDoubleShift) {
                    emit(0xf9000000u | (12u << 10) | (31 << 5) | 11);
                    mov(17,10);
                    doubleShiftResult(i,true);
                    return;
                }
                if (i.segmentLoad) {
                    segmentLoad(i,unsigned(n),beforeLoad);
                    if (i.stackDelta) {
                        // POP SS changes SS.B, but its pop used the OLD stack
                        // width. Commit that increment only after loading the
                        // descriptor succeeds; declines replay the entire pop.
                        emit(0x11000000u | (i.w << 10) | ((20+SP) << 5) | 11);
                        insert(20+SP,11,block.stack32 ? 4 : 2,0);
                        dirtyRegisters |= 1u << SP;
                    }
                    return;
                }
                if (i.stackMany) {
                    // Every access was guarded before the first read/write. Keep
                    // original SP until its saved slot is written; POPA skips it.
                    for (int slot = 0; slot < 8; ++slot) {
                        const int reg = 7 - slot;
                        const unsigned offset = slot * i.w;
                        if (i.memory.store)
                            emit((i.w == 4 ? 0xb9000000u : 0x79000000u) |
                                 ((offset / i.w) << 10) | (11 << 5) | (20 + reg));
                        else if (reg != SP) {
                            emit((i.w == 4 ? 0xb9400000u : 0x79400000u) |
                                 ((offset / i.w) << 10) | (11 << 5) | 10);
                            insert(20 + reg, 10, i.w, 0);
                        }
                    }
                    // Use a scratch distinct from the guarded host address.
                    emit((i.memory.store ? 0x51000000u : 0x11000000u) |
                         ((8 * i.w) << 10) | ((20 + SP) << 5) | 10);
                    insert(20 + SP, 10, block.stack32 ? 4 : 2, 0);
                    dirtyRegisters |= i.memory.store ? 1u << SP : 255u;
                    return;
                }
                if (i.memoryShift) {
                    emit(0xf9000000u | (12u << 10) | (31 << 5) | 11); // str RAM address,[sp,#96]
                    mov(17, 10);
                    auto operation = i;
                    operation.kind = i.op < 4 ? Kind::Rotate : Kind::Shift;
                    shiftResult(operation, true, true);
                    return;
                }
                if (i.memoryDivide) {
                    divideResult(i, divisionErrors);
                    return;
                }
                if(i.memoryMultiplyFull) {
                    mov(12,10);
                    multiplyFullResult(i,true);
                    return;
                }
                if (i.memoryMultiply) {
                    emit(0x13000000u | ((i.w * 8 - 1) << 10) | (10 << 5) | 11);
                    if (i.immediate) imm(12, i.imm);
                    else mov(12, 20 + i.dst);
                    emit(0x13000000u | ((i.w * 8 - 1) << 10) | (12 << 5) | 12);
                    multiplyResult(i, true, true);
                    return;
                }
                if (i.memory.bit >= 4) {
                    // Preserve every flag except CF, including undefined flags
                    // retained by the interpreter. Publish only after success.
                    if (i.immediate) imm(9, i.imm);
                    else bits(9, 20 + i.dst, 0, i.w == 2 ? 4 : 5);
                    emit(0x1ac02400u | (9 << 16) | (10 << 5) | 13); // lsrv old,index
                    load(12, 19, offsetof(CPU, flags));
                    flagBit(13, 0);
                    store(12, 19, offsetof(CPU, flags));
                    return;
                }
                if (i.memoryAlu) {
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
                    if (i.memoryResult) {
                        // The address/read completed with the original cached
                        // GPRs. Commit the register result only after success,
                        // retaining both original operands for precise flags.
                        uint32_t op = i.op == 0 || i.op == 2 ? 0x0b000000u
                                      : i.op == 1 ? 0x2a000000u
                                      : i.op == 4 ? 0x0a000000u
                                      : i.op == 6 ? 0x4a000000u : 0x4b000000u;
                        emit(op | (10 << 16) | (12 << 5) | 11);
                        if (memoryCarry) {
                            load(14, 31, 104);
                            emit((i.op == 2 ? 0x0b000000u : 0x4b000000u) | (14 << 16) | (11 << 5) | 11);
                        }
                        insert(20 + i.dst, 11, i.w, i.dstShift);
                        dirtyRegisters |= 1u << i.dst;
                    }
                    return;
                }
                if (i.popFlags) {
                    load(12,19,offsetof(CPU,flags));
                    const unsigned cpl = block.vm ? 3 : (block.cr0 & 1) ? cpu.s[CS].selector & 3 : 0;
                    unsigned change=0x7fd7;
                    if ((block.cr0 & 1) && cpl) change &= ~0x3000u;
                    imm(13,change);
                    if ((block.cr0 & 1) && cpl) {
                        bits(14,12,12,2);
                        emit(0x7100001fu | (cpl << 10) | (14 << 5));
                        const auto allowed=code.size();emit(0x54000002u); // b.hs: IOPL >= CPL
                        imm(15,~uint32_t(IF));
                        emit(0x0a000000u | (15 << 16) | (13 << 5) | 13);
                        code[allowed] |= uint32_t(code.size()-allowed) << 5;
                    }
                    emit(0x0a000000u | (13 << 16) | (10 << 5) | 10);
                    emit(0x0a200000u | (13 << 16) | (12 << 5) | 12);
                    emit(0x2a000000u | (10 << 16) | (12 << 5) | 12);
                    imm(14,2);
                    emit(0x2a000000u | (14 << 16) | (12 << 5) | 12);
                    store(12,19,offsetof(CPU,flags));
                    emit(0x11000000u | (i.w << 10) | ((20+SP) << 5) | 11);
                    insert(20+SP,11,block.stack32 ? 4 : 2,0);
                    dirtyRegisters |= 1u << SP;
                    return;
                }
                // Change SP only after a successful access. POP SP/ESP writes
                // its destination after the increment, including mixed widths.
                // LEAVE derives the incremented stack pointer from BP/EBP.
                if (i.stackDelta) {
                    auto amount = unsigned(i.stackDelta < 0 ? -i.stackDelta : i.stackDelta);
                    const int stackBase = i.leaveFrame ? BP : SP;
                    if (amount < 4096)
                        emit((i.stackDelta < 0 ? 0x51000000u : 0x11000000u) | (amount << 10) |
                             ((20 + stackBase) << 5) | 11);
                    else {
                        imm(12, amount);
                        emit(0x0b000000u | (12 << 16) | ((20 + stackBase) << 5) | 11);
                    }
                    insert(20 + SP, 11, block.stack32 ? 4 : 2, 0);
                    dirtyRegisters |= 1u << SP;
                }
                if (i.nearReturn || i.indirectJump || i.indirectCall) {
                    // Commit the dynamic destination only after a successful
                    // stack access or pointer read. Replay retains the original IP.
                    if (i.indirectCall) load(10, 31, 108);
                    store(10, 19, offsetof(CPU, ip));
                } else if (!i.memory.store || i.memory.exchange) {
                    if (i.signedSource)
                        emit(0x13000000u | ((i.sourceWidth * 8 - 1) << 10) | (10 << 5) | 10);
                    insert(20 + i.dst, 10, i.w, i.dstShift);
                    dirtyRegisters |= 1u << i.dst;
                }
                if (i.stringMove) {
                    // DF selects +/- element width. Preserve arithmetic flags
                    // and update SI/ESI or DI/EDI only after a successful access.
                    load(13, 19, offsetof(CPU, flags));
                    bits(13, 13, 10, 1);
                    imm(12, i.w);
                    const unsigned shift = i.w == 1 ? 1 : i.w == 2 ? 2 : 3;
                    emit(0x4b000000u | (13 << 16) | (shift << 10) | (12 << 5) | 12);
                    const int index = i.memory.base;
                    emit(0x0b000000u | (12 << 16) | ((20 + index) << 5) | 11);
                    insert(20 + index, 11, i.memory.address32 ? 4 : 2, 0);
                    dirtyRegisters |= 1u << index;
                }
            };
            imm(2, i.memory.displacement);
            if (i.memory.base >= 0)
                emit(0x0b000000u | ((20 + i.memory.base) << 16) | (2 << 5) | 2);
            if (i.memory.index >= 0) {
                int index=20+i.memory.index;
                if (i.xlat) {
                    extract(13,index,1,0);
                    index=13;
                }
                emit(0x0b000000u | (index << 16) | (i.memory.scale << 10) | (2 << 5) | 2);
            }
            if (!i.memory.address32)
                emit(0x53003c00u | (2 << 5) | 2);
            if (i.memory.bit >= 4 && !i.immediate) {
                // Signed register indexes address a bit string. Apply the word
                // displacement AFTER address-size wrapping; never wrap again
                // at 16 bits. Arithmetic shift implements floor for negatives.
                emit(0x13000000u | ((i.w * 8 - 1) << 10) | ((20 + i.dst) << 5) | 13);
                const unsigned shift = i.w == 2 ? 4 : 5;
                emit(0x13007c00u | (shift << 16) | (13 << 5) | 13); // asr w13,#shift
                emit(0x0b000000u | (13 << 16) | ((i.w == 2 ? 1 : 2) << 10) | (2 << 5) | 2);
            }
            // Keep w2 (the adjusted segment offset) intact for the slow helper.
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
                if (i.stackMany && !block.stack32) {
                    // Per-element stack wrapping must replay with interpreter
                    // ordering and partial-fault state, never become a flat span.
                    compareImmediate(2, 65536 - accessWidth);
                    guard(0x54000008u);
                }
                auto segmentOffset = offsetof(CPU, s) + i.memory.segment * sizeof(Segment);
                if ((block.cr0 & 1) && !block.vm) {
                    emit(0x39400000u | ((segmentOffset + offsetof(Segment, unusable)) << 10) |
                         (19 << 5) | 9); // ldrb w9,[x19,#unusable]
                    guard(0x35000009u);  // cbnz w9,slow
                }
                load(9, 19, segmentOffset + offsetof(Segment, base));
                emit(0x0b020129u); // add w9,w9,w2 (linear address, wraps at 32 bits)
                if (accessWidth > 1) {
                    emit(0x12002d2au); // and w10,w9,#4095
                    compareImmediate(10, 4096 - accessWidth);
                    guard(0x54000008u); // b.hi slow (cross-page access)
                }
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
                    // The little-endian tag/epoch pair forms one 64-bit key.
                    // A stale epoch or colliding page tag must both miss.
                    static_assert(std::endian::native == std::endian::little);
                    static_assert(offsetof(CPU::TLBEntry, tag) == 0);
                    static_assert(offsetof(CPU::TLBEntry, epoch) == 4);
                    emit(0xf940018du); // ldr x13,[x12]: epoch:tag (unaligned entries allowed)
                    int epoch = epochRegister;
                    if (epoch < 0) {
                        cpuAddress(14, offsetof(CPU, tlbEpoch) & ~4095u);
                        load(14, 14, offsetof(CPU, tlbEpoch) & 4095u);
                        epoch = 14;
                    }
                    emit(0xaa00814eu | (epoch << 16)); // orr x14,x10,xEpoch,lsl #32
                    emit(0xeb0e01bfu); // cmp x13,x14
                    guard(0x54000001u); // b.ne slow
                    bool user = block.vm || ((block.cr0 & 1) && (cpu.s[CS].selector & 3) == 3);
                    if (user) {
                        // User-accessible pages are also valid during a system
                        // access override. Other pages conservatively take the
                        // helper, which applies the override before faulting.
                        if (i.memory.store) {
                            static_assert(offsetof(CPU::TLBEntry, dirty) ==
                                          offsetof(CPU::TLBEntry, permissions) + 1);
                            static_assert(sizeof(bool) == 1);
                            emit(0x7940218du); // ldrh w13,[x12,#permissions]: dirty:permissions
                            compareImmediate(13, 0x106); // writable + user + dirty
                            guard(0x54000001u); // b.ne slow
                        } else {
                            emit(0x3940418du); // ldrb w13,[x12,#permissions]
                            guard(0x3610000du); // tbz w13,#2,slow (not user-readable)
                        }
                    }
                    if (i.memory.store && !user) {
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
                    // A byte, or a wider access passing the page-boundary guard,
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
                    if (accessWidth > 1 && !i.stackMany) {
                        emit(0x1200152au); // and w10,w9,#63
                        compareImmediate(10, 64 - accessWidth);
                        guard(0x54000008u); // b.hi slow (two code bitmap words)
                    }
                    emit(0x53067d2au);  // lsr w10,w9,#6
                    if (bitmapRegister >= 0)
                        emit(0x8b2a4c00u | (bitmapRegister << 5) | 12);
                    else {
                        pointer(12, reinterpret_cast<uintptr_t>(block.codeBits));
                        emit(0x8b2a4d8cu); // add x12,x12,w10,uxtw #3
                    }
                    emit(0xf940018au);  // ldr x10,[x12]
                    guard(0xb500000au); // cbnz x10,slow (possibly translated bytes)
                    if (i.stackMany) {
                        // The span can cover two 64-byte bitmap words. Check
                        // both, including unaligned stacks, before any store.
                        emit(0x11000000u | ((accessWidth - 1) << 10) | (9 << 5) | 10);
                        emit(0x4a000000u | (9 << 16) | (10 << 5) | 10);
                        bits(10, 10, 6, 1); // Last word is zero or one past first.
                        emit(0x8b2a4d8cu); // add x12,x12,w10,uxtw #3
                        emit(0xf940018au);
                        guard(0xb500000au);
                    }
                }
                emit(0x8b29438bu); // add x11,x28,w9,uxtw
                if (i.stackMany) {
                    // finishMove performs all accesses after the whole-span guard.
                } else if (i.memoryShift || i.memoryDoubleShift) {
                    emit((accessWidth == 4 ? 0xb9400000u : accessWidth == 2 ? 0x79400000u : 0x39400000u) |
                         (11 << 5) | 10);
                } else if (i.memory.delta || i.memory.alu >= 0 || i.memory.exchange || i.memory.bit > 4 || i.memory.invert) {
                    emit((accessWidth == 4 ? 0xb9400000u : accessWidth == 2 ? 0x79400000u : 0x39400000u) |
                         (11 << 5) | 10);
                    if (i.memory.invert)
                        emit(0x2a2003e0u | (10 << 16) | 13); // mvn result,old
                    else if (i.memory.bit > 4) {
                        if (i.immediate) imm(9, i.imm);
                        else bits(9, 20 + i.dst, 0, i.w == 2 ? 4 : 5);
                        imm(12, 1);
                        emit(0x1ac02000u | (9 << 16) | (12 << 5) | 12); // lslv mask,index
                        auto op = i.memory.bit == 5 ? 0x2a000000u : i.memory.bit == 6 ? 0x0a200000u : 0x4a000000u;
                        emit(op | (12 << 16) | (10 << 5) | 13);
                    } else if (i.memory.exchange)
                        extract(13, 20 + i.dst, i.w, i.dstShift);
                    else if (i.memory.delta)
                        emit((i.memory.delta > 0 ? 0x11000400u : 0x51000400u) | (10 << 5) | 13);
                    else {
                        if (i.immediate) imm(12, i.imm);
                        else extract(12, 20 + i.dst, i.w, i.dstShift);
                        uint32_t op = i.op == 0 || i.op == 2 ? 0x0b000000u : i.op == 1 ? 0x2a000000u
                                      : i.op == 4 ? 0x0a000000u : i.op == 6 ? 0x4a000000u
                                      : 0x4b000000u;
                        emit(op | (12 << 16) | (10 << 5) | 13);
                        if (memoryCarry) {
                            load(14, 31, 104);
                            emit((i.op == 2 ? 0x0b000000u : 0x4b000000u) | (14 << 16) | (13 << 5) | 13);
                        }
                    }
                    emit((accessWidth == 4 ? 0xb9000000u : accessWidth == 2 ? 0x79000000u : 0x39000000u) |
                         (11 << 5) | 13);
                } else if (i.memory.store) {
                    int source = 20 + i.dst;
                    if(i.setCondition>=0) {
                        load(10,31,104);
                        source=10;
                    } else if (i.pushFlags) {
                        load(10,19,offsetof(CPU,flags));
                        if (i.w == 4) {
                            imm(13,0x30000);
                            emit(0x0a200000u | (13 << 16) | (10 << 5) | 10);
                        }
                        source = 10;
                    } else if (i.segmentRegister >= 0) {
                        selector(10,i.segmentRegister);
                        source = 10;
                    } else if (i.memoryPush) {
                        load(10, 31, 108);
                        source = 10;
                    } else if (i.immediate) {
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
            if (i.memoryMultiplyFull || i.memoryDivide || i.memoryShift || i.memoryDoubleShift || i.stackMany || i.segmentLoad || i.popFlags || i.xlat) {
                // Cold/unsafe accesses replay BEFORE translation or device access.
                // Reading first and then replaying could repeat MMIO or
                // observe data/code changed by page-table accessed/dirty bits.
                for (auto at : divisionErrors) code[at] |= uint32_t(code.size() - at) << 5;
                replay(i.memory.begin, unsigned(n), beforeLoad);
                if (fastSuccess != size_t(-1)) code[fastSuccess] |= uint32_t(code.size() - fastSuccess);
                if (zeroDoubleEnd!=size_t(-1))code[zeroDoubleEnd] |= uint32_t(code.size()-zeroDoubleEnd);
                continue;
            }
            emit(0xaa1303e0u);
            pointer(1, reinterpret_cast<uintptr_t>(metadata));
            if(i.setCondition>=0)
                load(3,31,104);
            else if (i.pushFlags) {
                load(3,19,offsetof(CPU,flags));
                if (i.w == 4) {
                    imm(13,0x30000);
                    emit(0x0a200000u | (13 << 16) | (3 << 5) | 3);
                }
            } else if (i.segmentRegister >= 0)
                selector(3,i.segmentRegister);
            else if (i.memoryPush)
                load(3, 31, 108);
            else if (i.immediate)
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
            if (!i.memory.store || i.memory.delta || i.memory.alu >= 0 || i.memory.exchange || i.memory.bit > 4 || i.memory.invert) {
                load(10, 19, offsetof(CPU, nativeValue));
            }
            finishMove();
            emit(0x7100041fu); // cmp w0,#1: completed and code unchanged.
            auto continuation = code.size();
            emit(0x54000000u); // b.eq continuation
            // Successful memory access changed code: commit through this instruction.
            flushRegisters(dirtyRegisters);
            finishFlags(i.memoryAlu || i.memoryMultiply ? int(n) : last, -1, 0);
            if (!i.nearReturn && !i.indirectJump && !i.indirectCall) {
                imm(9, i.memory.end);
                store(9, 19, offsetof(CPU, ip));
            }
            imm(0, n + 1);
            exits.push_back(code.size());
            emit(0x14000000u);
            code[fallback] |= uint32_t(code.size() - fallback) << 5;
            if (!invalidCalls.empty()) {
                for (auto at : invalidCalls)
                    code[at] |= uint32_t(code.size() - at) << 5;
                static_assert(offsetof(CPU, nativeFallback) < 4096);
                imm(9, 1);
                emit(0x39000000u | (offsetof(CPU, nativeFallback) << 10) | (19 << 5) | 9);
            }
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
            if (i.memoryAlu || i.memoryMultiply)
                last = n;
            continue;
        }
        bool withCarry = i.kind == Kind::Alu && (i.op == 2 || i.op == 3);
        if (withCarry) {
            finishFlags(last, -1, 0);
            last = -1;
            load(13, 19, offsetof(CPU, flags));
            bits(13, 13, 0, 1);
            if (neededFlags[n]) store(13, 31, 104);
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
        if (i.negate)
            imm(11, 0);
        if (neededFlags[n]) {
            store(11, 31, 96);
            store(12, 31, 100);
        }
        if (i.kind == Kind::Move)
            mov(10, 12);
        else {
            uint32_t op = i.op == 0 || i.op == 2 || i.op == 8 ? 0x0b000000
                          : i.op == 1 ? 0x2a000000
                          : i.op == 4 ? 0x0a000000
                          : i.op == 6 ? 0x4a000000
                                      : 0x4b000000;
            emit(op | (12 << 16) | (11 << 5) | 10);
            if (withCarry)
                emit((i.op == 2 ? 0x0b000000u : 0x4b000000u) |
                     (13 << 16) | (10 << 5) | 10);
        }
        if (i.kind == Kind::Test || (i.kind == Kind::Alu && i.op == 7))
            continue;
        dirtyRegisters |= 1u << i.dst;
        insert(dst, 10, i.w, i.dstShift);
    }
    mark("GPR commit", ir.back().start);
    flushRegisters(dirtyRegisters);
    bool branch = ir.back().kind == Kind::Branch;
    mark("IP commit", ir.back().start);
    if (!ir.back().nearReturn && !ir.back().indirectJump && !ir.back().indirectCall &&
        ir.back().kind != Kind::CounterBranch && ir.back().kind != Kind::FarTransfer && ir.back().kind != Kind::FarReturn && ir.back().kind != Kind::PortIO && ir.back().kind != Kind::String) {
        imm(9, ir.back().nearCall ? ir.back().target
                                 : branch ? (ir.back().dst < 0 ? ir.back().imm : ir.back().end)
                                          : pos);
        store(9, 19, offsetof(CPU, ip));
    }
    mark("Flags and branch", ir.back().start);
    finishFlags(last, branch ? ir.back().dst : -1, branch ? ir.back().imm : 0);
    mark("Instruction count", ir.back().start);
    imm(0, terminal.kind==Kind::PortIO ? nativePortIOExit|uint32_t(ir.size()-1) : uint32_t(ir.size()));
    if (linkCandidate) {
        const auto &tail = ir.back();
        if (tail.kind == Kind::Branch || tail.kind == Kind::CounterBranch) {
            block.link()->ips[0] = tail.imm;
            block.link()->count = 1;
            if (tail.kind == Kind::CounterBranch || tail.dst >= 0) {
                block.link()->ips[1] = tail.end;
                block.link()->count = 2;
            }
        } else if (tail.nearCall && !tail.indirectCall) {
            block.link()->ips[0] = tail.target;
            block.link()->count = 1;
        } else if (!tail.nearReturn && !tail.indirectJump && !tail.indirectCall &&
                   tail.kind != Kind::IndirectJump) {
            block.link()->ips[0] = pos;
            block.link()->count = 1;
        }
        block.link()->fetchCheck = linkNeedsFetchCheck;
        block.link()->fullExitOffset = code.size()*4;
        emit(0xd503201fu); // A hot promotion redirects this to a small link stub.
        block.link()->earlyExitOffset = code.size()*4;
    }
    for (auto at : exits)
        code[at] |= uint32_t(code.size() - at) & 0x03ffffffu;
    mark("Host epilogue", ir.back().start);
    for (int k = 0; k < 5; k++)
        if (savePair(k))
            emit(0xa9400000u | (((16 + k * 16) / 8) << 15) | ((20 + k * 2) << 10) | (31 << 5) |
                 (19 + k * 2));
    emit(0xa8c77bfdu);
    emit(0xd65f03c0u); // ldp x29,x30,[sp],#128/#112; ret
    size_t bytes = code.size() * 4;
    if (bytes > capacity)
        return block; // Never copy an oversized block beyond the executable arena.
    if (used + bytes > capacity) {
        debugReset();
        incomingLinks.clear();
        blocks.clear();
        cacheRecycles++;
        fast = {};
        used = stubUsed = 0;
    }
    pthread_jit_write_protect_np(0);
    std::memcpy(arena + used, code.data(), bytes);
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate(arena + used, bytes);
    block.fn = reinterpret_cast<Function>(arena + used);
    used = (used + bytes + 15) & ~size_t(15);
    compiledBlocks++;
    if (block.link()) block.link()->codeBytes = bytes;
    debugBlock(block, code, debugRegions);
    if(decoded.fetchTail.present || terminal.kind==Kind::PortIO || terminal.kind==Kind::FarTransfer || terminal.kind==Kind::FarReturn) {
        block.native->boundary->fn=block.fn;
        block.fn=nullptr;
    }
    return block;
}
bool Jit::promoteBlock(Block &block) {
    auto &link = *block.link();
    std::vector<uint32_t> code;
    std::vector<DebugRegion> regions;
    auto emit = [&](uint32_t word) { code.push_back(word); };
    auto mark = [&](const char *kind, uint32_t ip) {
        if (debugState) regions.push_back({code.size()*4, kind, ip});
    };
    auto imm = [&](int reg, uint32_t value) {
        emit(0x52800000u | ((value & 65535) << 5) | reg);
        if (value >> 16) emit(0x72a00000u | ((value >> 16) << 5) | reg);
    };
    auto load = [&](int reg, int base, unsigned off) {
        emit(0xb9400000u | ((off/4) << 10) | (base << 5) | reg);
    };
    auto store = [&](int reg, int base, unsigned off) {
        emit(0xb9000000u | ((off/4) << 10) | (base << 5) | reg);
    };
    auto pointer = [&](int reg, uintptr_t value) {
        emit(0xd2800000u | ((value & 65535) << 5) | reg);
        for (unsigned part = 1; part < 4; ++part)
            emit(0xf2800000u | (part << 21) | (((value >> (part*16)) & 65535) << 5) | reg);
    };
    auto cpuAddress = [&](int reg, unsigned offset) {
        unsigned high = offset >> 12, low = offset & 4095;
        if (high) emit(0x91400000u | (high << 10) | (19 << 5) | reg);
        if (low || !high) emit(0x91000000u | (low << 10) | ((high ? reg : 19) << 5) | reg);
    };
    auto bits = [&](int dst, int src, unsigned first, unsigned count) {
        emit(0x53000000u | (first << 16) | ((first+count-1) << 10) | (src << 5) | dst);
    };
    mark("Linked root entry", link.startIP);
    emit(0xa9b87bfdu); // Shared 128-byte frame; 96..108 remain operand/carry spills.
    emit(0x910003fdu);
    for (int k = 0; k < 5; ++k)
        emit(0xa9000000u | (((16+k*16)/8) << 15) | ((20+k*2) << 10) | (31 << 5) | (19+k*2));
    emit(0xaa0003f3u);
    store(1, 31, 112);
    store(31, 31, 116);
    const auto enterBody = code.size(); emit(0x14000000u);
    const auto fullExit = code.size();
    std::vector<size_t> normalExit, packedExit;
    size_t earlyExit = 0;
    auto conditionalExit = [&](std::vector<size_t> &list, uint32_t instruction) {
        list.push_back(code.size()); emit(instruction);
    };
    auto aggregate = [&]() {
        load(9, 31, 112);
        conditionalExit(normalExit, 0x34000009u); // cbz w9, ordinary epilogue
        load(10, 31, 116);
        emit(0x0b00014au); // add w10,w10,w0: completed instructions
        emit(0x1140414au); // add w10,w10,#16,lsl #12: one logical dispatch
        store(10, 31, 116);
    };
    mark("Direct-link checks", block.link()->startIP);
    aggregate();
    emit(0x6b00013fu); // cmp w9,w0
    conditionalExit(packedExit, 0x54000009u); // b.ls: budget exhausted
    emit(0x4b000129u); // sub w9,w9,w0
    store(9, 31, 112);
    if (block.link()->count) {
        // Supported native operations do not change CPU execution context.
        // Recheck code identity and the cached fetch mapping at each edge;
        // any eviction/remap goes through ordinary permission/fault handling.
        // Every completed slow memory operation already checks source
        // code generation, and inline stores reject marked code bytes.
        // Only a slow memory helper can evict/change the fetch TLB slot.
        size_t noHelper = 0;
        if (block.link()->fetchCheck) {
            load(9, 31, 120);
            noHelper = code.size(); emit(0x34000009u);
            const uint32_t page = uint32_t(block.csBase + block.link()->startIP) >> 12;
            auto index = (page ^ (page >> 10)) & (cpu.tlb.size()-1);
            pointer(16, reinterpret_cast<uintptr_t>(&cpu.tlb[index]));
            load(11, 16, offsetof(CPU::TLBEntry, epoch));
            cpuAddress(17, offsetof(CPU, tlbEpoch));
            load(12, 17, 0);
            emit(0x6b0c017fu);
            conditionalExit(packedExit, 0x54000001u);
            load(11, 16, offsetof(CPU::TLBEntry, tag));
            imm(12, page);
            emit(0x6b0c017fu);
            conditionalExit(packedExit, 0x54000001u);
            load(11, 16, offsetof(CPU::TLBEntry, base));
            if (!block.a20) {
                imm(12, ~(1u << 20));
                emit(0x0a0c016bu); // and w11,w11,w12
            }
            imm(12, block.page << 12);
            emit(0x6b0c017fu);
            conditionalExit(packedExit, 0x54000001u);
            if (cpu.cpl() == 3) {
                emit(0x39400000u | (offsetof(CPU::TLBEntry, permissions) << 10) | (16 << 5) | 11);
                bits(11, 11, 2, 1);
                conditionalExit(packedExit, 0x3400000bu);
            }
            code[noHelper] |= uint32_t(code.size()-noHelper) << 5;
        }
        if (block.link()->count == 2) {
            load(11, 19, offsetof(CPU, ip));
            imm(12, block.link()->ips[0]);
            emit(0x6b0c017fu);
            emit(0x54000041u); // b.ne over the first unconditional link slot
        }
        for (unsigned slot = 0; slot < block.link()->count; ++slot) {
            block.link()->branchOffsets[slot] = code.size()*4;
            packedExit.push_back(code.size()); emit(0x14000000u);
        }
    } else {
        packedExit.push_back(code.size()); emit(0x14000000u);
    }
    earlyExit = code.size();
    aggregate(); // A partial prefix/replay must never follow a direct edge.
    auto nonzeroPrefix = code.size(); emit(0x35000000u); // cbnz w0, packed
    load(10, 31, 116);
    imm(11, 0x80000000u); // Carry a zero-ending prefix through the packed return.
    emit(0x2a0b014au); // orr w10,w10,w11
    store(10, 31, 116);
    code[nonzeroPrefix] |= uint32_t(code.size()-nonzeroPrefix) << 5;
    const auto packed = code.size();
    block.link()->fallbackOffset = packed*4;
    for (auto at : packedExit) {
        if ((code[at] & 0xfc000000u) == 0x14000000u)
            code[at] |= uint32_t(packed-at);
        else code[at] |= uint32_t(packed-at) << 5;
    }
    load(10, 31, 116);
    bits(0, 10, 0, 16);
    bits(1, 10, 16, 16);
    emit(0xaa018000u); // orr x0,x0,x1,lsl #32

    for (auto at : normalExit) code[at] |= uint32_t(code.size()-at) << 5;
    mark("Linked host epilogue", link.startIP);
    for (int k = 0; k < 5; ++k)
        emit(0xa9400000u | (((16+k*16)/8) << 15) | ((20+k*2) << 10) | (31 << 5) | (19+k*2));
    emit(0xa8c87bfdu); emit(0xd65f03c0u);
    const auto bytes = code.size()*4;
    if (bytes > stubCapacity-stubUsed) return false; // Never recycle while holding a block reference.
    auto start = reinterpret_cast<uintptr_t>(arena+capacity+stubUsed);
    auto base = reinterpret_cast<uintptr_t>(block.fn);
    auto branch = [](uintptr_t from, uintptr_t to) {
        return 0x14000000u | (uint32_t((int64_t(to)-int64_t(from))/4) & 0x03ffffffu);
    };
    code[enterBody] = branch(start+enterBody*4, base+link.entry);
    std::vector<std::pair<uintptr_t, uint32_t>> patches{
        {base, branch(base, start)},
        {base+link.fullExitOffset, branch(base+link.fullExitOffset, start+fullExit*4)},
        {base+link.earlyExitOffset, branch(base+link.earlyExitOffset, start+earlyExit*4)}};
    if (link.fetchCheck) {
        patches.push_back({base+link.resetFlagOffset,
                          0xb9000000u | ((120/4) << 10) | (31 << 5) | 31});
        for (auto at : link.helperFlagSites) {
            patches.push_back({base+at, 0xb9000000u | ((120/4) << 10) | (31 << 5)}); // str w0,[sp,#120]
        }
    }
    pthread_jit_write_protect_np(0);
    std::memcpy(reinterpret_cast<void *>(start), code.data(), bytes);
    for (auto [address, instruction] : patches) *reinterpret_cast<uint32_t *>(address) = instruction;
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate(reinterpret_cast<void *>(start), bytes);
    sys_icache_invalidate(reinterpret_cast<void *>(base), link.codeBytes);
    if (debugState) {
        debugBlock(block, code, regions, start);
        for (auto [address, instruction] : patches) debugPatch(address, instruction);
    }
    const auto delta = uint32_t(start-base);
    for (unsigned slot = 0; slot < link.count; ++slot) link.branchOffsets[slot] += delta;
    link.fallbackOffset += delta;
    link.saveMask = 31;
    link.promoted = true;
    stubUsed = (stubUsed+bytes+15) & ~size_t(15);
    ++directLinkBlocks;
    return true;
}
} // namespace pc
#endif
