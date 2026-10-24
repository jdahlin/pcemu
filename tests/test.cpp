#include "cpu.hpp"
#include "jit.hpp"
#include "machine.hpp"
#include "ps2_test_support.hpp"
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
using namespace pc;
static unsigned checks = 0;
static void check(bool b, const char *message) {
    checks++;
    if (!b)
        throw std::runtime_error(message);
}
static void program(CPU &c, std::initializer_list<uint8_t> bytes) {
    c.ip = 0;
    std::copy(bytes.begin(), bytes.end(), c.mem.begin());
}
int main() {
    try {
        PIC8259 pic;
        pic.mask = 0;
        pic.raise(3);
        pic.raise(1);
        check(pic.pending() == 1, "PIC fixed priority");
        pic.acknowledge(1);
        check(pic.pending() < 0, "PIC blocks lower priority while interrupt in service");
        pic.raise(0);
        check(pic.pending() == 0, "PIC allows higher priority nested request");
        pic.command(0x0b);
        check(pic.read() == 2 && pic.read() == 2, "OCW3 selects persistent ISR reads");
        pic.command(0x0a);
        check(pic.read() == 9, "OCW3 selects IRR independently of ISR");
        pic.command(0x61);
        check(pic.isr == 0 && pic.pending() == 0, "specific EOI clears selected in-service level");
        pic.command(0x11);
        pic.data(0x50);
        pic.data(4);
        pic.data(3);
        pic.data(0);
        pic.raise(1);
        pic.acknowledge(1);
        check(pic.base == 0x50 && pic.isr == 0, "ICW4 automatic EOI does not leave ISR set");
        pic.raise(4);
        pic.command(0x0c);
        check(pic.read() == 0x84 && pic.irr == 0, "PIC poll read acknowledges highest request");
        for (unsigned lowest = 0; lowest < 8; ++lowest)
            for (unsigned requests : {0u, 1u, 0x80u, 0x55u, 0xaau, 0xffu})
                for (unsigned service : {0u, 1u, 0x80u, 0x55u, 0xaau, 0xffu})
                    for (unsigned mask : {0u, 0xffu, 0x55u, 0xaau})
                        for (bool special : {false, true}) {
                            PIC8259 priority;
                            priority.lowest = lowest;
                            priority.irr = requests;
                            priority.isr = service;
                            priority.mask = mask;
                            priority.specialMask = special;
                            int expected = -1;
                            // Walk interrupt priority in hardware order: an
                            // in-service level blocks itself and lower levels.
                            for (unsigned rank = 0; rank < 8; ++rank) {
                                unsigned level = (lowest + 1 + rank) & 7;
                                unsigned bit = 1u << level;
                                if ((service & bit) && !(special && (mask & bit)))
                                    break;
                                if (requests & ~mask & bit) {
                                    expected = level;
                                    break;
                                }
                            }
                            check(priority.pending() == expected,
                                  "PIC priority respects rotation, masks, nesting and empty "
                                  "requests");
                        }
        PIT8254 pit;
        pit.write(3, 0x34, 0);
        pit.write(0, 0, 0);
        pit.write(0, 0, 0);
        pit.write(3, 0, 7);
        auto low = pit.read(0, 100);
        auto high = pit.read(0, 200);
        check((low | (high << 8)) == 65529, "PIT latch holds both bytes across time changes");
        pit.write(3, 0, 300);
        low = pit.read(0, 301);
        high = pit.read(0, 302);
        check((low | (high << 8)) == 65236, "subsequent PIT latch captures fresh count");
        pit.write(3, 0xe2, 400);
        check((pit.read(0, 400) & 0x3f) == 0x34, "8254 read-back latches channel status");
        check(!pit.tick(65535) && pit.tick(65536) && !pit.tick(65537),
              "PIT period creates a single rising edge");
        pit.write(3, 0xb6, 0);
        pit.write(2, 10, 0);
        pit.write(2, 0, 0);
        pit.counters[2].setGate(true, 0);
        check(pit.counters[2].output(4) && !pit.counters[2].output(5) && pit.counters[2].output(10),
              "PIT square-wave output and speaker gate");
        for (unsigned mode = 0; mode < 6; ++mode)
            for (unsigned reload : {1u, 2u, 3u, 17u, 65536u})
                for (bool gate : {false, true})
                    for (bool nullCount : {false, true}) {
                        PIT8254 scheduled;
                        auto &counter = scheduled.counters[0];
                        counter.mode = mode;
                        counter.load(uint16_t(reload), 0);
                        counter.gate = gate;
                        counter.nullCount = nullCount;
                        uint64_t previous = 0;
                        for (uint64_t now : {0u, 1u, 2u, 3u, 16u, 17u, 18u, 65535u,
                                             65536u, 65537u, 131072u, 2u, 0u, 65536u}) {
                            bool raised = false;
                            if (gate && !nullCount) {
                                uint64_t edges = mode == 2 || mode == 3 ? now / reload : now >= reload;
                                raised = edges > previous;
                                if (mode == 2 || mode == 3)
                                    previous = std::max(previous, edges);
                                else
                                    previous = edges;
                            }
                            check(scheduled.tick(now) == raised && counter.lastEdge == previous,
                                  "PIT deadline guard preserves edge counts, skipped periods and clock rewinds");
                        }
                        counter.load(3, 200000);
                        check(!scheduled.tick(200002) && scheduled.tick(200003) == gate,
                              "PIT reload immediately changes the next edge deadline");
                    }
        CPU c;
        for (unsigned a = 0; a < 256; a++)
            for (unsigned b = 0; b < 256; b++)
                for (unsigned carry = 0; carry < 2; carry++)
                    for (int op : {2, 3}) {
                        c.flags = 2 | carry;
                        auto result = c.alu(op, a, b, 1);
                        int full = op == 2 ? int(a + b + carry) : int(a) - int(b) - int(carry),
                            signedFull = op == 2 ? int(int8_t(a)) + int(int8_t(b)) + carry
                                                 : int(int8_t(a)) - int(int8_t(b)) - int(carry);
                        uint32_t expected = (full < 0 || full > 255 ? CF : 0) |
                                            (signedFull < -128 || signedFull > 127 ? OF : 0) |
                                            ((full & 255) == 0 ? ZF : 0) | ((full & 128) ? SF : 0) |
                                            (__builtin_parity(full & 255) ? 0 : PF) |
                                            (((a ^ b ^ result) & 16) ? AF : 0);
                        check(result == (full & 255u) &&
                                  ((c.flags ^ (expected | 2)) & (CF | OF | ZF | SF | PF | AF)) == 0,
                              "exhaustive byte ADC/SBB flags");
                    }

        check(c.alu(0, 0x7fff, 1, 2) == 0x8000 && (c.flags & OF) && !(c.flags & CF),
              "signed addition flags");
        check(c.alu(0, 65535, 1, 2) == 0 && (c.flags & (CF | ZF | PF)) == (CF | ZF | PF),
              "unsigned addition flags");
        check(c.alu(5, 0, 1, 2) == 65535 && (c.flags & CF) && (c.flags & SF), "subtraction flags");
        c.flags = CF;
        check(c.alu(2, 65535, 0, 2) == 0 && (c.flags & CF), "ADC carry");
        c.flags = CF;
        check(c.alu(3, 0, 65535, 2) == 0 && (c.flags & CF), "SBB full-width borrow");
        c.r[AX] = 0x12345678;
        c.set(4, 0xab, 1);
        check(c.r[AX] == 0x1234ab78, "AH alias");
        c.set(AX, 0x9876, 2);
        check(c.r[AX] == 0x12349876, "AX preserves high word");
        c.segment(SS, 0x1000);
        c.r[SP] = 0x100;
        c.push(0xcafe, 2);
        check(c.read(0x100fe, 2) == 0xcafe && c.pop(2) == 0xcafe && c.r[SP] == 0x100,
              "real-mode stack");
        program(c, {0xb8, 0x34, 0x12, 0xbb, 0x00, 0x10, 0xbe, 0x20, 0x00, 0x89, 0x00, 0x8b, 0x08,
                    0xf4});
        while (!c.halted)
            c.step();
        check(c.read(0x1020, 2) == 0x1234 && c.get(CX, 2) == 0x1234, "ModRM BX+SI addressing");
        c.halted = false;
        c.r[CX] = 3;
        c.r[SI] = 0x200;
        c.r[DI] = 0x300;
        c.write(0x200, 0x434241, 4);
        program(c, {0xf3, 0xa4});
        c.step();
        check(c.read(0x300, 3) == 0x434241 && c.r[CX] == 0 && c.r[SI] == 0x203, "REP MOVSB");
        for (unsigned kind : {0xa4u, 0xa6u, 0xaau, 0xacu, 0xaeu, 0x6cu, 0x6eu})
            for (int width : {1, 2, 4})
                for (uint8_t prefix : {0xf3, 0xf2})
                    for (bool reverse : {false, true})
                        for (bool address32 : {false, true})
                            for (bool sourceFault : {false, true}) {
                                if (prefix == 0xf2 && kind != 0xa6 && kind != 0xae)
                                    continue;
                                bool source =
                                    kind == 0xa4 || kind == 0xa6 || kind == 0xac || kind == 0x6e;
                                bool destination = kind == 0xa4 || kind == 0xa6 || kind == 0xaa ||
                                                   kind == 0xae || kind == 0x6c;
                                if ((sourceFault && !source) || (!sourceFault && !destination))
                                    continue;
                                CPU repeated(1024 * 1024);
                                repeated.s[CS].big = true;
                                auto sourceStart = reverse ? 0x6000u + width : 0x6000u - 2 * width;
                                auto destStart = reverse ? 0x9000u + width : 0x9000u - 2 * width;
                                auto high = address32 ? 0u : 0xabcd0000u;
                                repeated.r[SI] = high | sourceStart;
                                repeated.r[DI] = high | destStart;
                                repeated.r[CX] = high | 4;
                                repeated.r[AX] = kind == 0xac ? 0x22222222 : 0x11111111;
                                repeated.r[DX] = 0x80;
                                auto initialFlags = 0x3202u | CF | SF | OF | (reverse ? DF : 0);
                                repeated.flags = initialFlags;
                                std::fill(repeated.mem.begin() + 0x5000,
                                          repeated.mem.begin() + 0x7000, 0x11);
                                std::fill(repeated.mem.begin() + 0x8000,
                                          repeated.mem.begin() + 0xa000,
                                          prefix == 0xf2 ? 0x22 : 0x11);
                                std::vector<uint8_t> code{prefix};
                                if (width == 2)
                                    code.push_back(0x66);
                                if (!address32)
                                    code.push_back(0x67);
                                code.push_back(kind | (width != 1));
                                std::copy(code.begin(), code.end(), repeated.mem.begin());
                                repeated.write(0x80000, 0x81007, 4);
                                for (unsigned page : {0u, 5u, 6u, 8u, 9u})
                                    repeated.write(0x81000 + page * 4, page * 4096 | 7, 4);
                                unsigned missing =
                                    sourceFault ? (reverse ? 5 : 6) : (reverse ? 8 : 9);
                                repeated.write(0x81000 + missing * 4, 0, 4);
                                repeated.cr[0] = 0x80000011;
                                repeated.cr[3] = 0x80000;
                                repeated.s[CS].selector = 3;
                                unsigned outputs = 0;
                                repeated.input = [](uint16_t, int) { return 0x11111111u; };
                                repeated.output = [&](uint16_t, uint32_t, int) { outputs++; };
                                struct Stop {};
                                bool seen = false;
                                auto next = [&](uint32_t begin, unsigned elements) {
                                    return high | ((begin + (reverse ? -width : width) * elements) &
                                                   (address32 ? 0xffffffffu : 65535u));
                                };
                                auto expectedAX = kind == 0xac ? (width == 4   ? 0x11111111u
                                                                  : width == 2 ? 0x22221111u
                                                                               : 0x22222211u)
                                                               : 0x11111111u;
                                repeated.observeFault = [&](const Fault &fault) {
                                    auto expectedSI =
                                        source ? next(sourceStart, 2) : high | sourceStart;
                                    auto expectedDI =
                                        destination ? next(destStart, 2) : high | destStart;
                                    auto faultAddress = (sourceFault ? expectedSI : expectedDI) &
                                                        (address32 ? 0xffffffffu : 65535u);
                                    seen = fault.vector == 14 && fault.partialRepeat &&
                                           repeated.ip == 0 && repeated.r[CX] == (high | 2) &&
                                           repeated.r[SI] == expectedSI &&
                                           repeated.r[DI] == expectedDI &&
                                           repeated.r[AX] == expectedAX &&
                                           repeated.flags == initialFlags &&
                                           repeated.cr[2] == faultAddress;
                                    throw Stop{};
                                };
                                try {
                                    repeated.step();
                                } catch (const Stop &) {
                                }
                                check(seen,
                                      "REP fault preserves completed elements and original flags");
                                repeated.observeFault = {};
                                uint32_t mapping = missing * 4096 | 7;
                                std::memcpy(repeated.mem.data() + 0x81000 + missing * 4, &mapping,
                                            4);
                                repeated.flushTLB();
                                repeated.step();
                                check(repeated.r[CX] == high && repeated.ip == code.size() &&
                                          repeated.r[SI] == (source ? next(sourceStart, 4)
                                                                    : high | sourceStart) &&
                                          repeated.r[DI] == (destination ? next(destStart, 4)
                                                                         : high | destStart) &&
                                          (kind != 0x6e || outputs == 4),
                                      "REP resumes only unfinished elements after the fault is "
                                      "repaired");
                            }
        {
            CPU longCompare(1024 * 1024);
            longCompare.s[CS].big = true;
            program(longCompare, {0xf3, 0xa6});
            longCompare.write(0x80000, 0x81007, 4);
            for (unsigned page : {0u, 5u, 6u, 8u, 9u})
                longCompare.write(0x81000 + page * 4, page * 4096 | 7, 4);
            longCompare.cr[0] = 0x80000011;
            longCompare.cr[3] = 0x80000;
            longCompare.s[CS].selector = 3;
            longCompare.r[SI] = 0x5000;
            longCompare.r[DI] = 0x8000;
            longCompare.r[CX] = 8193;
            auto initialFlags = longCompare.flags = 2 | CF | SF | OF;
            longCompare.step();
            check(longCompare.ip == 0 && longCompare.r[CX] == 4097 && (longCompare.flags & ZF),
                  "long REP comparison yields for host scheduling");
            struct Stop {};
            bool seen = false;
            longCompare.observeFault = [&](const Fault &fault) {
                seen = fault.vector == 14 && longCompare.flags == initialFlags &&
                       longCompare.r[CX] == 1 && longCompare.r[SI] == 0x7000 &&
                       longCompare.r[DI] == 0xa000;
                throw Stop{};
            };
            try {
                longCompare.step();
                longCompare.step();
            } catch (const Stop &) {
            }
            check(seen, "REP faults after scheduling yields restore original comparison flags");
        }
        c.halted = false;
        c.r[AX] = 0xffff;
        c.flags = CF;
        program(c, {0x40});
        c.step();
        check(c.get(AX, 2) == 0 && (c.flags & CF) && (c.flags & ZF), "INC preserves carry");
        // Minimal valid GDT with a flat 32-bit code/data descriptor.
        c.gdtBase = 0x1000;
        c.gdtLimit = 23;
        c.write(0x1008, 0xffff, 4);
        c.write(0x100c, 0x00cf9a00, 4);
        c.write(0x1010, 0xffff, 4);
        c.write(0x1014, 0x00cf9200, 4);
        c.cr[0] |= 1;
        c.segment(CS, 8);
        c.segment(DS, 16);
        check(c.s[CS].big && c.s[CS].limit == 0xffffffff && c.s[DS].base == 0,
              "protected-mode descriptor cache");
        c.cr[3] = 0x2000;
        c.write(0x2000, 0x3003, 4);
        c.write(0x3000 + 4, 0x5003, 4);
        c.cr[0] |= 0x80000000;
        c.write(0x1234, 0xabcd, 2);
        check(c.mem[0x5234] == 0xcd && c.mem[0x5235] == 0xab && (c.read(0x1234, 2) == 0xabcd),
              "two-level page translation");
        c.cr[0] &= ~0x80000000u;
        std::mt19937 rng(42);
        // LEA must form offsets even for unusable segments and unmapped addresses.
        // Exhaustively vary registers/modes, and randomize SIBs, displacements and aliases.
        {
            CPU ref(65536), native(65536);
            Jit jit(native, true, 0, 65536);
            for (unsigned trial = 0; trial < 4096; ++trial) {
                bool big = trial & 1, operandOverride = trial & 2, addressOverride = trial & 4;
                bool address32 = big != addressOverride;
                unsigned dst = (trial >> 3) & 7, rm = (trial >> 6) & 7, mod = (trial >> 9) % 3;
                std::vector<uint8_t> code = {0x64}; // FS override must not cause a segment access.
                if (operandOverride)
                    code.push_back(0x66);
                if (addressOverride)
                    code.push_back(0x67);
                code.push_back(0x8d);
                code.push_back((mod << 6) | (dst << 3) | rm);
                unsigned base = rm;
                if (address32 && rm == 4) {
                    auto sib = uint8_t(rng());
                    code.push_back(sib);
                    base = sib & 7;
                }
                unsigned bytes = mod == 1                            ? 1
                                 : mod == 2                          ? (address32 ? 4 : 2)
                                 : (address32 ? base == 5 : rm == 6) ? (address32 ? 4 : 2)
                                                                     : 0;
                for (unsigned n = 0; n < bytes; ++n)
                    code.push_back(rng());
                code.push_back(0x70 | (trial & 15));
                code.push_back(0);
                for (auto c : {&ref, &native}) {
                    c->ip = 0;
                    c->instructions = 0;
                    c->halted = false;
                    c->cr[0] = 1;
                    c->s[CS].big = big;
                    for (int seg : {DS, SS, FS}) {
                        c->s[seg].base = 0xfff00000;
                        c->s[seg].unusable = true;
                    }
                    for (unsigned n = 0; n < code.size(); ++n)
                        c->write(n, code[n], 1);
                }
                ref.flags = native.flags = 2 | (rng() & 0x8d5);
                for (unsigned reg = 0; reg < 8; ++reg)
                    ref.r[reg] = native.r[reg] = rng();
                ref.step();
                ref.step();
                check(jit.run() == 2 && native.instructions == 2, "LEA executes natively");
                check(native.ip == ref.ip && native.flags == ref.flags &&
                          native.nativeMemoryCalls == 0,
                      "LEA preserves flags and does not access unusable segments or RAM");
                for (unsigned reg = 0; reg < 8; ++reg)
                    check(native.r[reg] == ref.r[reg],
                          "LEA SIB/width/displacement/alias matches interpreter");
            }
        }
        // Differential native execution with randomized inputs, sizes, ALU operations,
        // partial-register writes and final flags. Same bytes run on both engines.
        for (int trial = 0; trial < 1000; trial++) {
            CPU ref(1024 * 1024), native(1024 * 1024);
            ref.s[CS].big = native.s[CS].big = trial & 1;
            for (int reg = 0; reg < 8; reg++)
                ref.r[reg] = native.r[reg] = rng();
            ref.flags = native.flags = 2 | (rng() & 0xcd5);
            unsigned p = 0;
            for (int n = 0; n < 32; n++) {
                bool overrideSize = rng() % 4 == 0;
                if (overrideSize)
                    ref.mem[p++] = 0x66;
                int width = (bool(trial & 1) ^ overrideSize) ? 4 : 2;
                int kind = rng() % 4, dst = rng() % 8, src = rng() % 8;
                const uint8_t alu[] = {1, 9, 0x21, 0x29, 0x31, 0x39};
                if (kind == 0) {
                    ref.mem[p++] = 0xb8 + dst;
                    auto v = rng();
                    for (int k = 0; k < width; k++)
                        ref.mem[p++] = v >> (k * 8);
                } else {
                    ref.mem[p++] = kind == 1 ? 0x89 : alu[rng() % 6];
                    ref.mem[p++] = 0xc0 | (src << 3) | dst;
                }
            }
            if (trial & 2) {
                std::copy_n(ref.mem.begin(), p, ref.mem.begin() + 0x3000);
                ref.write(0x1000, 0x2007, 4);
                ref.write(0x2000, 0x3007, 4);
                ref.cr[3] = native.cr[3] = 0x1000;
                ref.cr[0] = native.cr[0] = 0x80000011;
            }
            native.mem = ref.mem;
            Jit jit(native, true, 0);
            for (int n = 0; n < 32; n++)
                ref.step();
            while (native.instructions < 32)
                jit.run();
            check(ref.ip == native.ip && ref.flags == native.flags,
                  "JIT flags and IP match interpreter");
            for (int reg = 0; reg < 8; reg++)
                check(ref.r[reg] == native.r[reg], "JIT registers match interpreter");
            // Rewrite translated guest code and verify code-cache invalidation.
            native.ip = 0;
            // Rewrite to a valid terminating instruction: replacing only a MOV
            // opcode with NOP would execute its random immediate as new code.
            native.write(0, 0xf4, 1);
            ref.ip = 0;
            ref.write(0, 0xf4, 1);
            auto executed = jit.run();
            for (unsigned i = 0; i < executed; i++)
                ref.step();
            check(native.ip == ref.ip && native.flags == ref.flags && native.halted == ref.halted,
                  "JIT invalidates modified code page");
            for (int reg = 0; reg < 8; reg++)
                check(native.r[reg] == ref.r[reg], "JIT invalidation register state");
        }
        for (bool operand8 : {false, true})
            for (unsigned trial = 0; trial < 512; trial++) {
                CPU ref(1024 * 1024), native(1024 * 1024);
                bool code32 = trial & 1, operand32 = trial & 2, address32 = trial & 4;
                ref.s[CS].big = native.s[CS].big = code32;
                for (int reg = 0; reg < 8; reg++)
                    ref.r[reg] = native.r[reg] =
                        0x400 + reg * 0x40 + (rng() & 31) | (address32 ? 0 : rng() & 0xffff0000u);
                for (int seg = 0; seg < 6; seg++)
                    ref.s[seg].base = native.s[seg].base = seg == CS ? 0 : (seg + 1) * 0x10000;
                for (unsigned at = 0; at < 0x70000; at++)
                    ref.mem[at] = uint8_t(at * 73 + trial);
                unsigned rm = (trial >> 3) & 7, mod = (trial >> 6) % 3;
                bool moffs = trial % 11 == 0;
                std::vector<uint8_t> addressing;
                unsigned addressRegisters = 0;
                auto append = [&](uint32_t value, int width) {
                    for (int n = 0; n < width; n++)
                        addressing.push_back(value >> (n * 8));
                };
                bool direct = false;
                if (address32) {
                    unsigned base = rm;
                    if (rm == 4) {
                        base = trial & 16 ? BP : SP;
                        unsigned index = trial & 32 ? SI : DI;
                        addressing.push_back(((trial >> 8) & 3) << 6 | index << 3 | base);
                        addressRegisters |= 1u << index;
                    }
                    direct = mod == 0 && base == 5;
                    if (!direct)
                        addressRegisters |= 1u << base;
                } else {
                    const unsigned regs[] = {(1u << BX) | (1u << SI),
                                             (1u << BX) | (1u << DI),
                                             (1u << BP) | (1u << SI),
                                             (1u << BP) | (1u << DI),
                                             1u << SI,
                                             1u << DI,
                                             1u << BP,
                                             1u << BX};
                    direct = mod == 0 && rm == 6;
                    if (!direct)
                        addressRegisters = regs[rm];
                }
                if (direct)
                    append(0x700, address32 ? 4 : 2);
                if (mod == 1)
                    append(0xf0, 1);
                else if (mod == 2)
                    append(0x600, address32 ? 4 : 2);
                int target = AX;
                while (addressRegisters & (1u << target))
                    target++;
                if (moffs)
                    target = AX;
                if (operand8 && !moffs && (trial & 16))
                    target |= 4;
                std::vector<uint8_t> code;
                auto memory = [&](bool store, int reg) {
                    if (operand32 != code32)
                        code.push_back(0x66);
                    if (address32 != code32)
                        code.push_back(0x67);
                    const uint8_t segments[] = {0x26, 0x2e, 0x36, 0x3e, 0x64, 0x65};
                    if (trial % 7 < 6)
                        code.push_back(segments[trial % 7]);
                    if (moffs) {
                        code.push_back(operand8 ? (store ? 0xa2 : 0xa0) : (store ? 0xa3 : 0xa1));
                        for (unsigned n = 0; n < (address32 ? 4u : 2u); n++)
                            code.push_back(0x700u >> (n * 8));
                    } else {
                        code.push_back(operand8 ? (store ? 0x88 : 0x8a) : (store ? 0x89 : 0x8b));
                        code.push_back(mod << 6 | reg << 3 | rm);
                        code.insert(code.end(), addressing.begin(), addressing.end());
                    }
                };
                memory(true, moffs ? AX : operand8 && (trial & 32) ? 5 : CX);
                memory(false, target);
                if (operand32 != code32)
                    code.push_back(0x66);
                code.insert(code.end(),
                            {uint8_t(operand8 ? 0x80 : 0x83), uint8_t(0xc0 | target), 7});
                memory(true, target);
                memory(false, target);
                if (operand32 != code32)
                    code.push_back(0x66);
                code.insert(code.end(), {uint8_t(operand8 ? 0x84 : 0x85),
                                         uint8_t(0xc0 | target << 3 | target), 0x75, 0});
                code.push_back(0xf4);
                std::copy(code.begin(), code.end(), ref.mem.begin());
                if (trial & 8) {
                    ref.write(0x80000, 0x81007, 4);
                    for (unsigned page = 0; page < 256; page++)
                        ref.write(0x81000 + page * 4, page * 4096 | 7, 4);
                    ref.cr[3] = native.cr[3] = 0x80000;
                    ref.cr[0] = native.cr[0] = 0x80000011;
                    ref.s[CS].selector = native.s[CS].selector = 3;
                }
                native.mem = ref.mem;
                Jit jit(native, true, 0);
                for (unsigned n = 0; n < 7; n++)
                    ref.step();
                while (native.instructions < 7)
                    jit.run();
                check(native.ip == ref.ip && native.flags == ref.flags && native.mem == ref.mem,
                      "native memory moves match all address forms, segments, widths and paging");
                if (operand8)
                    check(jit.nativeInstructions == 7,
                          "all byte memory instructions execute natively");
                for (unsigned reg = 0; reg < 8; reg++)
                    check(native.r[reg] == ref.r[reg], "native memory preserves exact GPR state");
            }
        // A packed native TLB key must distinguish page collisions from epoch
        // changes, including high epoch bits and wraparound. Odd table slots
        // also exercise the 20-byte entry stride's unaligned 64-bit key load.
        for (bool byte : {false, true})
            for (bool allRegisters : {false, true})
            for (uint32_t epoch : {1u, 0x80000001u, UINT32_MAX}) {
                CPU hot(1024 * 1024);
                hot.s[CS].big = true;
                program(hot, {uint8_t(byte ? 0xa0 : 0xa1), 0, 0x50, 0, 0,
                              uint8_t(byte ? 0x80 : 0x83), 0xc0, 1,
                              uint8_t(byte ? 0xa2 : 0xa3), 0, 0x50, 0, 0, 0xeb, 0xf1});
                if (allRegisters) {
                    std::memmove(hot.mem.data() + 16, hot.mem.data(), 15);
                    for (unsigned reg = 0; reg < 8; ++reg) {
                        hot.mem[reg * 2] = 0x89;
                        hot.mem[reg * 2 + 1] = 0xc0 | (reg << 3) | reg;
                    }
                    hot.mem[30] = uint8_t(-31);
                }
                unsigned blockInstructions = allRegisters ? 12 : 4;
                for (unsigned reg = 1; reg < 8; ++reg)
                    hot.r[reg] = 0x12345000 + reg;
                hot.write(0x80000, 0x81007, 4);
                hot.write(0x80004, 0x82007, 4);
                hot.write(0x81000, 7, 4);
                hot.write(0x81014, 0x5007, 4);
                hot.write(0x82010, 0x6007, 4);
                hot.cr[0] = 0x80000011;
                hot.cr[3] = 0x80000;
                hot.s[CS].selector = 3;
                hot.flushTLB();
                hot.tlbEpoch = epoch;
                Jit jit(hot, true, 0);
                check(jit.run() == blockInstructions && hot.mem[0x5000] == 1 && hot.nativeMemoryCalls == 2,
                      "native TLB key accepts full-width epochs after cold access");
                hot.physical(0x404000, true); // Hashes to the same slot as page 5.
                check(jit.run() == blockInstructions && hot.mem[0x5000] == 2 && hot.mem[0x6000] == 0 &&
                          hot.nativeMemoryCalls == 3,
                      "same-epoch TLB tag collision cannot reuse another page's RAM");
                uint32_t mapping = 0x7007;
                std::memcpy(hot.mem.data() + 0x81014, &mapping, 4);
                hot.flushTLB();
                check(jit.run() == blockInstructions && hot.mem[0x7000] == 1 && hot.mem[0x5000] == 2 &&
                          hot.nativeMemoryCalls == 5 &&
                          hot.tlbEpoch == (epoch == UINT32_MAX ? 1u : epoch + 1),
                      "same-tag TLB epoch change and wrap observe the remapped page");
                for (unsigned reg = 1; reg < 8; ++reg)
                    check(hot.r[reg] == 0x12345000 + reg,
                          "TLB scratch allocation preserves unused and fully cached guest GPRs");
            }
        for (bool byte : {false, true})
            for (bool paging : {false, true}) {
                CPU hot(1024 * 1024);
                hot.s[CS].big = true;
                program(hot,
                        {uint8_t(byte ? 0xa0 : 0xa1), 0, 0x50, 0, 0, uint8_t(byte ? 0x80 : 0x83),
                         0xc0, 1, uint8_t(byte ? 0xa2 : 0xa3), 0, 0x50, 0, 0, 0xeb, 0xf1});
                if (paging) {
                    hot.write(0x80000, 0x81007, 4);
                    hot.write(0x81000, 7, 4);
                    hot.write(0x81014, 0x5007, 4);
                    hot.cr[0] = 0x80000011;
                    hot.cr[3] = 0x80000;
                    hot.s[CS].selector = 3;
                }
                Jit jit(hot, true, 0);
                check(jit.run() == 4 && hot.ip == 0 && hot.read(0x5000, 4) == 1,
                      "inline RAM block completes one checked load/add/store loop");
                check(hot.nativeMemoryCalls == (paging ? 2u : 0u),
                      "only a cold TLB miss and dirty-bit update call the RAM helper");
                auto calls = hot.nativeMemoryCalls;
                for (unsigned n = 0; n < 20; n++)
                    jit.run();
                check(hot.nativeMemoryCalls == calls && hot.read(0x5000, 4) == 21,
                      "hot paged and flat RAM accesses run without helper calls");
                if (paging) {
                    // Data remapping must take effect even if the instruction page is unchanged.
                    uint32_t mapping = 0x6007;
                    std::memcpy(hot.mem.data() + 0x81014, &mapping, 4);
                    hot.flushTLB();
                    jit.run();
                    check(hot.read(0x5000, 4) == 1 && hot.mem[0x5000] == 21 &&
                              hot.mem[0x6000] == 1 && hot.nativeMemoryCalls == calls + 2,
                          "inline RAM TLB guards honor remapping and fresh dirty-bit updates");
                } else {
                    // Embedding host RAM/bitmap pointers requires invalidation after relocation.
                    hot.mem.reserve(hot.mem.capacity() * 2);
                    hot.codeUsed.reserve(hot.codeUsed.capacity() * 2);
                    jit.run();
                    check(hot.read(0x5000, 4) == 22 && hot.nativeMemoryCalls == calls,
                          "native RAM and code bitmap pointers refresh after host relocation");
                    hot.s[DS].base = 0x1000;
                    jit.run();
                    check(hot.read(0x6000, 4) == 1 && hot.read(0x5000, 4) == 22,
                          "cached inline memory uses the current segment base");
                    hot.markCode(0x6000);
                    auto generation = hot.generation[6];
                    jit.run();
                    check(hot.nativeMemoryCalls == calls + 1 &&
                              hot.generation[6] == generation + 1 && hot.read(0x6000, 4) == 2,
                          "newly translated data bytes force a code-invalidating slow store");
                    unsigned observed = 0;
                    hot.watchStart = 0x6000;
                    hot.watchLength = 4;
                    hot.watchWrite = [&](uint32_t, uint32_t, int) { observed++; };
                    // Interpreter replay returns before the trailing branch.
                    jit.run();
                    check(observed == 1 && hot.read(0x6000, 4) == 3 && hot.ip == 13,
                          "adding a write observer invalidates inline store assumptions");
                }
            }
        for (bool paging : {false, true})
            for (bool a20 : {false, true})
                for (int width : {2, 4})
                    for (uint32_t address : {0x503cu, 0x503du, 0x503fu, 0x5ffdu, 0x5ffeu, 0x9fffdu,
                                             0xafffeu, 0xb0000u, 0xffffeu, 0x105000u, 0x1ffffeu}) {
                        CPU ref(2 * 1024 * 1024), native(2 * 1024 * 1024);
                        unsigned reads[2]{}, writes[2]{};
                        for (unsigned n = 0; n < 2; n++) {
                            auto &c = n ? native : ref;
                            c.a20 = a20;
                            c.s[CS].big = true;
                            c.flags = 2 | CF | OF;
                            c.write(address, 0x7fffffffu, width);
                            std::vector<uint8_t> code;
                            auto memory = [&](bool store) {
                                if (width == 2)
                                    code.push_back(0x66);
                                code.push_back(store ? 0xa3 : 0xa1);
                                for (unsigned byte = 0; byte < 4; byte++)
                                    code.push_back(address >> (byte * 8));
                            };
                            memory(false);
                            if (width == 2)
                                code.push_back(0x66);
                            code.insert(code.end(), {0x83, 0xc0, 7});
                            memory(true);
                            memory(false);
                            code.insert(code.end(), {0x89, 0xc3, 0x90, 0xf4});
                            std::copy(code.begin(), code.end(), c.mem.begin());
                            if (paging) {
                                c.write(0x80000, 0x81007, 4);
                                for (unsigned page = 0; page <= 512; page++)
                                    c.write(0x81000 + page * 4, page * 4096 | 7, 4);
                                c.cr[3] = 0x80000;
                                c.cr[0] = 0x80000011;
                                c.s[CS].selector = 3;
                                // Warm both sides, including the second page of a split access.
                                c.physical(address, true);
                                c.physical(address + width - 1, true);
                            }
                            c.videoRead = [&, n](uint32_t) {
                                reads[n]++;
                                return uint8_t(0x5a);
                            };
                            c.videoWrite = [&, n](uint32_t, uint8_t value) {
                                writes[n] += value + 1;
                            };
                        }
                        for (unsigned n = 0; n < 6; n++)
                            ref.step();
                        Jit jit(native, true, 0);
                        while (native.instructions < 6)
                            jit.run();
                        check(native.mem == ref.mem && native.ip == ref.ip &&
                                  native.flags == ref.flags && reads[0] == reads[1] &&
                                  writes[0] == writes[1],
                              "inline memory preserves split accesses, bitmap boundaries, A20, "
                              "MMIO and outside-RAM effects");
                        for (unsigned reg = 0; reg < 8; reg++)
                            check(native.r[reg] == ref.r[reg],
                                  "inline memory boundary cases preserve all guest registers");
                    }
        for (int width : {2, 4})
            for (uint32_t address : {65535u, 65536u, 65537u, 65539u}) {
                CPU ref(65539), native(65539);
                for (auto c : {&ref, &native}) {
                    c->s[CS].big = true;
                    c->r[AX] = 0x12345678;
                    std::vector<uint8_t> code;
                    for (uint8_t op : {0xa3, 0xa1}) {
                        if (width == 2)
                            code.push_back(0x66);
                        code.push_back(op);
                        for (unsigned byte = 0; byte < 4; byte++)
                            code.push_back(address >> (byte * 8));
                    }
                    code.insert(code.end(), {0x89, 0xc3, 0x90, 0xf4});
                    std::copy(code.begin(), code.end(), c->mem.begin());
                }
                for (unsigned n = 0; n < 4; n++)
                    ref.step();
                Jit jit(native, true, 0);
                while (native.instructions < 4)
                    jit.run();
                check(native.mem == ref.mem && native.r[AX] == ref.r[AX] &&
                          native.r[BX] == ref.r[BX] && native.ip == ref.ip,
                      "inline bounds preserve partial accesses in non-power-of-two RAM");
            }
        for (bool store : {false, true}) {
            CPU c(1024 * 1024);
            c.s[CS].big = true;
            program(c, {0xb8, 1, 0, 0, 0, 0x83, 0xc0, 1, uint8_t(store ? 0xa3 : 0xa1), 0, 0x90, 0,
                        0, 0xf4});
            c.write(0x80000, 0x81007, 4);
            c.write(0x81000, 7, 4);
            c.write(0x81024, 0x9041 | (store ? 4 : 2), 4);
            c.cr[0] = 0x80000011;
            c.cr[3] = 0x80000;
            // Fill as supervisor, then exercise a user access to this same TLB hit.
            c.physical(0x9000);
            c.s[CS].selector = 3;
            struct Stop {};
            bool seen = false;
            c.observeFault = [&](const Fault &fault) {
                seen = fault.vector == 14 && fault.error == (store ? 7u : 5u) &&
                       c.cr[2] == 0x9000 && c.ip == 8 && c.r[AX] == 2;
                throw Stop{};
            };
            Jit jit(c, true, 0);
            try {
                jit.run();
            } catch (const Stop &) {
            }
            check(seen && c.nativeMemoryCalls == 1 && c.instructions == 3,
                  "prewarmed supervisor-only and read-only TLB hits enforce user permissions");
        }
        for (bool vm : {false, true}) {
            CPU c(1024 * 1024);
            c.s[CS].big = true;
            program(c, {0xb8, 0x78, 0x56, 0x34, 0x12, 0x90, 0xa3, 0, 0x90, 0, 0, 0xf4});
            c.write(0x80000, 0x81007, 4);
            c.write(0x81000, 7, 4);
            c.write(0x81024, 0x9041, 4); // Present, supervisor, read-only, already dirty.
            c.cr[0] = 0x80000011;
            c.cr[3] = 0x80000;
            c.physical(0x9000);
            if (vm) {
                c.flags |= 0x20000;
                c.systemAccess = 1;
            }
            Jit jit(c, true, 0);
            check(jit.run() == 3 && c.nativeMemoryCalls == (vm ? 1u : 0u) &&
                      c.mem[0x9000] == 0x78 && c.mem[0x9003] == 0x12,
                  "386 supervisor writes ignore read-only permissions and system access "
                  "overrides VM86 user checks");
        }
        for (bool store : {false, true})
            for (bool user : {false, true})
                for (unsigned kind = 0; kind < 3; kind++) {
                    if (kind == 1 && !user)
                        continue;
                    CPU ref(1024 * 1024), native(1024 * 1024);
                    uint32_t address = kind == 2 ? 0x9ffe : 0x9000;
                    uint32_t initial = store ? 0xffffffffu : 0x7fffffffu;
                    const uint8_t code[] = {0xb8,
                                            uint8_t(initial),
                                            uint8_t(initial >> 8),
                                            uint8_t(initial >> 16),
                                            uint8_t(initial >> 24),
                                            0x83,
                                            0xc0,
                                            1,
                                            uint8_t(store ? 0x89 : 0x8b),
                                            uint8_t(store ? 0x0d : 0x15),
                                            uint8_t(address),
                                            uint8_t(address >> 8),
                                            0,
                                            0,
                                            0xbb,
                                            0xef,
                                            0xbe,
                                            0xad,
                                            0xde,
                                            0xf4};
                    std::copy(std::begin(code), std::end(code), ref.mem.begin() + 0x100);
                    ref.write(0x80000, 0x81007, 4);
                    ref.write(0x81000, 7, 4);
                    if (kind)
                        ref.write(0x81000 + 9 * 4, 0x9000 | (kind == 2 ? 7 : store ? 5 : 3), 4);
                    native.mem = ref.mem;
                    for (auto c : {&ref, &native}) {
                        c->s[CS].big = c->s[SS].big = true;
                        c->s[CS].selector = user ? 3 : 0;
                        c->cr[0] = 0x80000011;
                        c->cr[3] = 0x80000;
                        c->r[CX] = 0xa1b2c3d4;
                        c->flags = 0x8d7;
                        c->ip = 0x100;
                    }
                    struct Stop {};
                    unsigned seen = 0;
                    auto observe = [&](CPU &c, const Fault &event) {
                        check(event.vector == 14 &&
                                  event.error == ((kind == 1 ? 1u : 0u) | (store ? 2u : 0u) |
                                                  (user ? 4u : 0u)) &&
                                  c.cr[2] == (kind == 2 ? 0xa000u : 0x9000u) && c.ip == 0x108 &&
                                  c.r[AX] == initial + 1 && c.r[BX] == 0,
                              "native memory fallback reports precise fault IP, CR2 and prior GPR "
                              "state");
                        seen++;
                        throw Stop{};
                    };
                    ref.observeFault = [&](const Fault &event) { observe(ref, event); };
                    native.observeFault = [&](const Fault &event) { observe(native, event); };
                    try {
                        for (unsigned n = 0; n < 3; n++)
                            ref.step();
                    } catch (const Stop &) {
                    }
                    Jit jit(native, true, 0);
                    try {
                        jit.run();
                    } catch (const Stop &) {
                    }
                    check(seen == 2 && native.instructions == 3 && jit.nativeInstructions == 2 &&
                              native.flags == ref.flags && native.mem == ref.mem,
                          "native fault replay preserves flags, partial cross-page stores and "
                          "instruction counts");
                }
        {
            CPU ref(1024 * 1024), native(1024 * 1024);
            program(ref, {0xb8, 0xff, 0xff, 0xff, 0xff, 0x83, 0xc0, 1, 0xa3, 0, 0x50, 0,
                          0,    0x8b, 0x1b, 0x8b, 0x13, 0x8b, 0x0d, 0, 0x90, 0, 0,    0xf4});
            ref.write(0x5000, 0xdeadbeef, 4);
            ref.write(0x5004, 0x6000, 4);
            ref.write(0x6000, 0x7fffffff, 4);
            ref.write(0x80000, 0x81007, 4);
            for (unsigned page : {0u, 5u, 6u})
                ref.write(0x81000 + page * 4, page * 4096 | 7, 4);
            native.mem = ref.mem;
            for (auto c : {&ref, &native}) {
                c->s[CS].big = true;
                c->cr[0] = 0x80000011;
                c->cr[3] = 0x80000;
                c->r[BX] = 0x5004;
                c->r[CX] = 0x10203040;
                c->flags = 2 | SF | OF;
            }
            struct Stop {};
            unsigned seen = 0;
            auto observe = [&](CPU &c, const Fault &fault) {
                check(fault.vector == 14 && c.ip == 17 && c.r[AX] == 0 && c.r[BX] == 0x6000 &&
                          c.r[DX] == 0x7fffffff && c.r[CX] == 0x10203040 &&
                          c.read(0x5000, 4) == 0 && (c.flags & (CF | ZF | SF | OF)) == (CF | ZF),
                      "cold exit commits successful aliasing loads, stores and deferred flags "
                      "before a later fault");
                seen++;
                throw Stop{};
            };
            ref.observeFault = [&](const Fault &fault) { observe(ref, fault); };
            native.observeFault = [&](const Fault &fault) { observe(native, fault); };
            try {
                for (unsigned n = 0; n < 6; n++)
                    ref.step();
            } catch (const Stop &) {
            }
            Jit jit(native, true, 0);
            try {
                jit.run();
            } catch (const Stop &) {
            }
            check(seen == 2 && native.instructions == 6 && jit.nativeInstructions == 5 &&
                      native.mem == ref.mem && native.flags == ref.flags,
                  "cached-register memory chains remain precise across multiple successful guards");
        }
        {
            CPU smc(65536);
            smc.s[CS].big = true;
            program(smc, {0xb8, 0x55, 0x44, 0x33, 0x22, 0x89, 0x05, 12, 0, 0, 0, 0xbb, 0x11, 0x11,
                          0x11, 0x11, 0x90, 0xf4});
            Jit jit(smc, true, 0);
            check(jit.run() == 2 && smc.ip == 11 && smc.r[BX] == 0,
                  "native store exits immediately when it changes translated future code");
            check(jit.run() == 2 && smc.r[BX] == 0x22334455,
                  "next native dispatch recompiles the modified immediate");
        }
        for (bool store : {false, true}) {
            CPU c(1024 * 1024);
            c.s[CS].big = true;
            unsigned accesses = 0;
            c.videoRead = [&](uint32_t) {
                accesses++;
                return uint8_t(0x12);
            };
            c.videoWrite = [&](uint32_t, uint8_t value) {
                accesses++;
                check(value == 0x78 || value == 0x56 || value == 0x34 || value == 0x12,
                      "MMIO store preserves byte values on interpreter fallback");
            };
            program(c, {0xb8, 0x78, 0x56, 0x34, 0x12, 0x90, uint8_t(store ? 0xa3 : 0xa1), 0, 0,
                        0x0a, 0, 0xbb, 0, 0, 0, 0, 0xf4});
            Jit jit(c, true, 0);
            check(jit.run() == 3 && accesses == 4 && c.ip == 11 && jit.nativeInstructions == 2,
                  "native MMIO guard replays once without duplicate device effects");
            if (!store)
                check(c.r[AX] == 0x12121212, "MMIO fallback returns actual device data");
        }
        {
            CPU watched(65536);
            watched.s[CS].big = true;
            program(watched, {0xb8, 0x78, 0x56, 0x34, 0x12, 0x90, 0xa3, 0, 0x90, 0, 0, 0xbb, 0, 0,
                              0, 0, 0xf4});
            watched.watchStart = 0x9000;
            watched.watchLength = 4;
            unsigned observed = 0;
            watched.watchWrite = [&](uint32_t address, uint32_t value, int width) {
                observed++;
                check(
                    address == 0x9000 && value == 0x12345678 && width == 4 && watched.ip == 11,
                    "watched native store replays with the interpreter's exact observation point");
            };
            Jit jit(watched, true, 0);
            check(jit.run() == 3 && observed == 1 && watched.read(0x9000, 4) == 0x12345678,
                  "native memory guards do not bypass or duplicate write observers");
        }
        {
            CPU nullData(65536);
            nullData.s[CS].big = true;
            nullData.cr[0] = 0x11;
            nullData.cr[2] = 0x13579;
            nullData.s[DS].unusable = true;
            program(nullData, {0xb8, 0x78, 0x56, 0x34, 0x12, 0x90, 0xa1, 0, 0x90, 0, 0, 0xf4});
            struct Stop {};
            bool seen = false;
            nullData.observeFault = [&](const Fault &fault) {
                seen = fault.vector == 13 && fault.error == 0 && nullData.ip == 6 &&
                       nullData.cr[2] == 0x13579 && nullData.r[AX] == 0x12345678;
                throw Stop{};
            };
            Jit jit(nullData, true, 0);
            try {
                jit.run();
            } catch (const Stop &) {
            }
            check(seen && nullData.instructions == 3 && jit.nativeInstructions == 2,
                  "native memory access through an unusable segment replays an exact GP fault");
        }
        {
            CPU recycleMemory(256 * 1024);
            recycleMemory.s[CS].big = true;
            const uint8_t code[] = {0xa1, 0, 0, 2, 0, 0x83, 0xc0, 1, 0xa3, 0, 0, 2, 0, 0xf4};
            for (unsigned block = 0; block < 1024; block++)
                std::copy(std::begin(code), std::end(code), recycleMemory.mem.begin() + block * 32);
            Jit jit(recycleMemory, true, 0, 65536);
            for (unsigned block = 0; block < 1024; block++) {
                recycleMemory.ip = block * 32;
                check(jit.run() == 3, "memory blocks execute through repeated arena recycling");
            }
            auto compiled = jit.compiledBlocks;
            recycleMemory.ip = 1023 * 32;
            jit.run();
            check(jit.cacheRecycles > 0 && jit.compiledBlocks == compiled &&
                      recycleMemory.read(0x20000, 4) == 1025,
                  "cached memory operand metadata survives arena reuse without dangling pointers");
            recycleMemory.ip = 0;
            jit.run();
            check(recycleMemory.read(0x20000, 4) == 1026,
                  "evicted memory block recompiles with live operand metadata");
        }
        // Branches terminate native blocks and consume the last arithmetic flags,
        // including branches that use flags inherited from a previous block.
        for (int mode = 0; mode < 2; mode++)
            for (int cc = 0; cc < 16; cc++)
                for (unsigned bits = 0; bits < 32; bits++) {
                    CPU ref(65536), native(65536);
                    ref.s[CS].big = native.s[CS].big = mode;
                    ref.flags = native.flags = 2 | ((bits & 1) ? CF : 0) | ((bits & 2) ? ZF : 0) |
                                               ((bits & 4) ? SF : 0) | ((bits & 8) ? OF : 0) |
                                               ((bits & 16) ? PF : 0);
                    ref.mem[0] = 0x90;
                    ref.mem[1] = 0x90;
                    unsigned end = 2;
                    if (bits & 1) {
                        ref.mem[end++] = 0x0f;
                        ref.mem[end++] = 0x80 | cc;
                        ref.mem[end++] = 0x20;
                        ref.mem[end++] = 0;
                        if (mode) {
                            ref.mem[end++] = 0;
                            ref.mem[end++] = 0;
                        }
                    } else {
                        ref.mem[end++] = 0x70 | cc;
                        ref.mem[end++] = 0xfc; // backward edge to the first NOP
                    }
                    native.mem = ref.mem;
                    Jit jit(native, true, 0);
                    for (int n = 0; n < 3; n++)
                        ref.step();
                    auto n = jit.run();
                    check(n == 3 && native.ip == ref.ip && native.flags == ref.flags,
                          "native conditional branch matches all inherited condition flags");
                    // CMP updates flags before the same branch is evaluated.
                    ref.ip = native.ip = 0;
                    ref.mem[0] = native.mem[0] = 0x39;
                    ref.mem[1] = native.mem[1] = 0xd8; // cmp ax/eax,bx/ebx
                    ref.r[AX] = native.r[AX] = bits * 0x11111111u;
                    ref.r[BX] = native.r[BX] = 0x80008000;
                    native.written(0, 2);
                    ref.step();
                    ref.step();
                    n = jit.run();
                    check(n == 2 && native.ip == ref.ip && native.flags == ref.flags,
                          "native branch consumes flags produced in its own block");
                }
        for (int mode = 0; mode < 2; mode++)
            for (bool shortBranch : {false, true}) {
                CPU ref(65536), native(65536);
                ref.s[CS].big = native.s[CS].big = mode;
                ref.flags = native.flags = 0xad7;
                ref.mem[0] = 0x90;
                ref.mem[1] = 0x90;
                ref.mem[2] = shortBranch ? 0xeb : 0xe9;
                ref.mem[3] = shortBranch ? 0xfc : 0xfb;
                if (!shortBranch) {
                    ref.mem[4] = 0xff;
                    if (mode) {
                        ref.mem[3] = 0xf9;
                        ref.mem[5] = ref.mem[6] = 0xff;
                    }
                }
                native.mem = ref.mem;
                Jit jit(native, true, 0);
                for (int n = 0; n < 3; n++)
                    ref.step();
                auto n = jit.run();
                check(n == 3 && native.ip == 0 && native.ip == ref.ip && native.flags == ref.flags,
                      "native short and near unconditional branches preserve flags");
            }
        {
            CPU paged;
            paged.s[CS].big = true;
            paged.write(0x1000, 0x2007, 4);
            paged.write(0x2000, 0x3007, 4);
            paged.write(0x3000, 0x111111b8, 4);
            paged.write(0x3004, 0x11, 1);
            paged.write(0x4000, 0x222222b8, 4);
            paged.write(0x4004, 0x22, 1);
            paged.cr[3] = 0x1000;
            paged.cr[0] = 0x80000011;
            Jit jit(paged, true, 0);
            jit.run();
            check(paged.r[AX] == 0x11111111, "paged native block executes mapped code");
            auto compiled = jit.compiledBlocks;
            paged.ip = 0;
            paged.flushTLB();
            jit.run();
            check(jit.compiledBlocks == compiled && paged.r[AX] == 0x11111111,
                  "TLB flush with unchanged code mapping reuses native code after checking "
                  "permissions");
            paged.ip = 0;
            // Update the PTE through its physical backing, as a guest page-table
            // store followed by INVLPG would do.
            paged.mem[0x2001] = 0x40;
            paged.flushTLB();
            jit.run();
            check(paged.r[AX] == 0x22222222,
                  "native cache invalidates remapped code after TLB flush");
            paged.s[CS].selector = 3;
            paged.ip = 0;
            jit.run();
            paged.mem[0x2000] = 3; // Present, supervisor-only code mapping.
            paged.flushTLB();
            paged.ip = 0;
            bool denied = false;
            struct Stop {};
            paged.observeFault = [&](const Fault &fault) {
                denied = fault.vector == 14 && fault.error == 5 && paged.ip == 0;
                throw Stop{};
            };
            try {
                jit.run();
            } catch (const Stop &) {
            }
            check(denied,
                  "cached native code cannot bypass changed fetch permissions after TLB flush");
        }
        {
            CPU hot;
            program(hot, {0xb8, 1, 0, 0xb9, 3, 0, 0xba, 4, 0});
            Jit jit(hot);
            for (unsigned n = 0; n < 7; n++) {
                hot.ip = 0;
                jit.run();
            }
            check(jit.nativeInstructions == 0, "cold code executes before native compilation");
            hot.write(1, 2, 2);
            hot.ip = 0;
            jit.run();
            check(hot.get(AX, 2) == 2 && (!jit.enabled() || jit.nativeInstructions > 0),
                  "hot compilation reads code modified during warmup");
        }
        {
            CPU recycle;
            recycle.s[CS].big = true;
            Jit jit(recycle, true, 0, 65536);
            if (jit.enabled()) {
                for (unsigned n = 0; n < 2048; n++) {
                    unsigned at = n * 16;
                    for (unsigned reg = 0; reg < 3; reg++) {
                        recycle.write(at + reg * 5, 0xb8 + reg, 1);
                        recycle.write(at + reg * 5 + 1, n + reg, 4);
                    }
                    recycle.write(at + 15, 0xf4, 1);
                    recycle.ip = at;
                    jit.run();
                }
                check(jit.cacheRecycles > 0,
                      "native code arena recycles under sustained compilation");
                recycle.ip = 0;
                recycle.write(1, 0xdead, 4);
                jit.run();
                check(recycle.r[AX] == 0xdead && recycle.r[CX] == 1,
                      "fast dispatch cache survives arena recycling and code modification");
            }
        }
        {
            CPU query;
            query.write(0x1000, 0x2007, 4);
            query.write(0x2000, 0x3007, 4);
            query.write(0x2018, 0x6003, 4);     // GDT is supervisor-only.
            query.write(0x3000, 0x0fc8030f, 4); // LSL ECX,AX; LAR EDX,AX.
            query.write(0x3004, 0xd002, 2);
            query.write(0x6008, 0xffff, 4);
            query.write(0x600c, 0x00cff200, 4);
            query.gdtBase = 0x6000;
            query.gdtLimit = 15;
            query.s[CS].selector = 3;
            query.s[CS].big = true;
            query.r[AX] = 11;
            query.cr[3] = 0x1000;
            query.cr[0] = 0x80000011;
            query.step();
            check((query.flags & ZF) && query.r[CX] == 0xffffffff,
                  "user LSL reads descriptor table with supervisor access");
            query.step();
            check((query.flags & ZF) && (query.r[DX] & 0xff00) == 0xf200,
                  "user LAR reads descriptor table with supervisor access");
            auto descriptor = [&](uint32_t high) {
                query.systemAccess++;
                query.write(0x600c, high, 4);
                query.systemAccess--;
            };
            for (uint32_t high : {0x00cf9200u, 0u, 0x00cfe400u}) {
                descriptor(high);
                query.ip = 0;
                query.r[CX] = 0xdeadbeef;
                query.step();
                check(!(query.flags & ZF) && query.r[CX] == 0xdeadbeef,
                      "LSL preserves destination for invisible or invalid descriptor type");
            }
            descriptor(0x00cf9e00); // Readable conforming ring-0 code.
            query.ip = 0;
            query.step();
            check((query.flags & ZF) && query.r[CX] == 0xffffffff,
                  "LSL accepts conforming code at lower DPL");
            query.mem[0x3000] = 0x0f;
            query.mem[0x3001] = 0;
            query.mem[0x3002] = 0xe0; // VERR AX.
            descriptor(0x00cff000);   // Read-only user data.
            query.ip = 0;
            query.step();
            check(query.flags & ZF, "VERR accepts read-only user data");
            query.mem[0x3002] = 0xe8; // VERW AX.
            query.ip = 0;
            query.step();
            check(!(query.flags & ZF), "VERW rejects read-only data");
            descriptor(0x00cff200);
            query.ip = 0;
            query.step();
            check(query.flags & ZF, "VERW accepts writable user data");
            descriptor(0x00cffa00); // Readable user code cannot be written.
            query.ip = 0;
            query.step();
            check(!(query.flags & ZF), "VERW rejects code segments");
        }
        for (bool stack16 : {false, true}) {
            CPU gate;
            gate.gdtBase = 0x6000;
            gate.gdtLimit = 47;
            for (unsigned entry : {8u, 16u, 32u, 40u}) {
                gate.write(0x6000 + entry, 0xffff, 4);
                gate.write(0x6004 + entry,
                           (entry == 8    ? 0x00cf9a00
                            : entry == 16 ? (stack16 ? 0x009200 : 0x00cf9200)
                            : entry == 32 ? 0x00cffa00
                                          : (stack16 ? 0x00f200 : 0x00cff200)),
                           4);
            }
            gate.write(0x6018, 0x00080500, 4);
            gate.write(0x601c, 0xec02, 4); // DPL3 32-bit call gate, two parameters.
            gate.trSelector = 0x30;
            gate.trBase = 0x7000;
            gate.tr32 = true;
            gate.write(0x7004, stack16 ? 0xabcd9000 : 0x9000, 4);
            gate.write(0x7008, 16, 2);
            gate.write(0, 0x9a, 1);
            gate.write(1, 0, 4);
            gate.write(5, 0x1b, 2);
            gate.write(0x500, 0x0008ca, 3); // RETF 8 restores caller and parameters.
            gate.write(0x600, 0xcf, 1);     // 32-bit IRET from a privilege-changing IRQ.
            gate.idtBase = 0x3000;
            gate.idtLimit = 0x7ff;
            gate.write(0x3000 + 0x30 * 8, 0x00080600, 4);
            gate.write(0x3004 + 0x30 * 8, 0x8e00, 4);
            gate.write(0xb100, 0x11223344, 4);
            gate.write(0xb104, 0x55667788, 4);
            gate.write(0x1000, 0x2007, 4);
            for (unsigned page : {0u, 3u, 6u, 7u, 8u, 11u})
                gate.write(0x2000 + page * 4, page * 4096 | (page == 0 || page == 11 ? 7 : 3), 4);
            gate.cr[0] = 0x11;
            gate.segment(SS, 0x2b);
            gate.segment(CS, 0x23);
            gate.r[SP] = stack16 ? 0x1234b100 : 0xb100;
            gate.cr[3] = 0x1000;
            gate.cr[0] |= 0x80000000;
            gate.step();
            check(gate.cpl() == 0 && gate.ip == 0x500 &&
                      gate.r[SP] == (stack16 ? 0xabcd8fe8u : 0x8fe8u) &&
                      gate.read(0x8ff0, 4) == 0x11223344 && gate.read(0x8ff4, 4) == 0x55667788,
                  "call gate copies user parameters onto supervisor-only privilege stack");
            gate.step();
            check(gate.cpl() == 3 && gate.ip == 7 &&
                      gate.r[SP] == (stack16 ? 0x1234b108u : 0xb108u) &&
                      gate.s[SS].selector == 0x2b,
                  "call gate far return restores caller stack and removes parameters");
            gate.interrupt(0x30);
            check(gate.cpl() == 0 && gate.r[SP] == (stack16 ? 0xabcd8fecu : 0x8fecu) &&
                      gate.read(0x8ff8, 4) == (stack16 ? 0x1234b108u : 0xb108u),
                  "32-bit interrupt preserves full ESP with either stack address size");
            gate.step();
            check(gate.cpl() == 3 && gate.ip == 7 &&
                      gate.r[SP] == (stack16 ? 0x1234b108u : 0xb108u),
                  "IRETD restores full ESP to a 16-bit or 32-bit outer stack");
        }
        for (bool code32 : {false, true}) {
            CPU popMemory;
            popMemory.s[CS].big = code32;
            popMemory.s[SS].big = code32;
            if (code32)
                program(popMemory, {0x8f, 0x44, 0x24, 0x08});
            else
                program(popMemory, {0x67, 0x66, 0x8f, 0x44, 0x24, 0x08});
            popMemory.r[SP] = 0x1000;
            popMemory.write(0x1000, 0x11223344, 4);
            popMemory.write(0x1008, 0xaabbccdd, 4);
            popMemory.step();
            check(popMemory.r[SP] == 0x1004 && popMemory.read(0x100c, 4) == 0x11223344 &&
                      popMemory.read(0x1008, 4) == 0xaabbccdd,
                  "POP ESP-relative memory calculates destination after stack increment");
        }
        {
            CPU nullSegment;
            nullSegment.segment(ES, 0); // Hidden real-mode state survives enabling PE.
            nullSegment.cr[0] |= 1;
            nullSegment.segment(DS, 0);
            program(nullSegment, {0xaa});
            nullSegment.r[AX] = 'A';
            nullSegment.r[DI] = 0x100;
            nullSegment.step();
            check(nullSegment.read(0x100, 1) == 'A',
                  "STOS does not reference an unusable source segment");
            nullSegment.segment(ES, 0);
            bool rejected = false;
            try {
                nullSegment.address(ES, 0);
            } catch (const Fault &f) {
                rejected = f.vector == 13 && f.error == 0;
            }
            check(rejected, "protected-mode null data selector faults on use");
            nullSegment.flags |= 0x20000;
            nullSegment.segment(ES, 0);
            check(nullSegment.address(ES, 0) == 0,
                  "VM86 segment zero remains a usable real-mode address");
        }
        CPU fault;
        fault.gdtBase = 0x1000;
        fault.gdtLimit = 31;
        fault.write(0x1008, 0xffff, 4);
        fault.write(0x100c, 0x009a00, 4);
        fault.write(0x1010, 0xffff, 4);
        fault.write(0x1014, 0x001200, 4);
        fault.write(0x1018, 0xffff, 4);
        fault.write(0x101c, 0x009200, 4);
        fault.cr[0] |= 1;
        fault.segment(CS, 8);
        fault.segment(SS, 24);
        fault.r[SP] = 0x8000;
        fault.r[AX] = 16;
        fault.ip = 0x100;
        fault.write(0x100, 0xd88e, 2);
        fault.idtBase = 0x2000;
        fault.idtLimit = 0x7ff;
        fault.write(0x2000 + 11 * 8, 0x00080400, 4);
        fault.write(0x2004 + 11 * 8, 0x8600, 4);
        unsigned observedFaults = 0;
        fault.observeFault = [&](const Fault &event) {
            observedFaults++;
            check(event.vector == 11 && event.error == 16 && fault.ip == 0x100 &&
                      fault.s[DS].selector == 0 && fault.r[SP] == 0x8000,
                  "fault observer sees precise restarted state before guest delivery");
        };
        fault.step();
        check(observedFaults == 1, "fault observer called once per delivered exception");
        check(fault.ip == 0x400 && fault.s[DS].selector == 0 && fault.pop(2) == 16 &&
                  fault.pop(2) == 0x100,
              "segment-not-present fault restarts at MOV and pushes selector");
        for (auto [seg, access] :
             {std::pair{DS, 0x000f0000u}, std::pair{DS, 0x00008000u}, std::pair{ES, 0x00001800u},
              std::pair{ES, 0x00009800u}, std::pair{SS, 0x00001000u}, std::pair{CS, 0x00001200u}}) {
            fault.write(0x1014, access, 4);
            auto old = fault.s[seg];
            bool rejected = false;
            try {
                fault.segment(seg, 16);
            } catch (const Fault &event) {
                rejected = event.vector == 13 && event.error == 16;
            }
            check(rejected && fault.s[seg].selector == old.selector &&
                      fault.s[seg].base == old.base,
                  "invalid segment type raises GP before NP and leaves hidden cache intact");
        }
        CPU vm;
        vm.gdtBase = 0x6000;
        vm.gdtLimit = 31;
        vm.write(0x6008, 0xffff, 4);
        vm.write(0x600c, 0x00cf9a00, 4);
        vm.write(0x6010, 0xffff, 4);
        vm.write(0x6014, 0x00cf9200, 4);
        vm.cr[0] |= 1;
        vm.trSelector = 24;
        vm.trBase = 0x3000;
        vm.trLimit = 0x100;
        vm.tr32 = true;
        vm.write(0x3004, 0x9000, 4);
        vm.write(0x3008, 16, 2);
        vm.idtBase = 0x2000;
        vm.idtLimit = 2047;
        vm.write(0x2000 + 13 * 8, 0x00080500, 4);
        vm.write(0x2004 + 13 * 8, 0x8e00, 4);
        vm.flags = 0x20202;
        vm.segment(CS, 0x100);
        vm.segment(SS, 0x200);
        vm.segment(ES, 0x300);
        vm.segment(DS, 0x400);
        vm.segment(FS, 0x500);
        vm.segment(GS, 0x600);
        vm.ip = 0x10;
        vm.r[SP] = 0x12340800;
        vm.write(0x1010, 0xfa, 1);
        vm.write(0x500, 0x04c483, 3);
        vm.write(0x503, 0xcf, 1);
        vm.step();
        check(vm.ip == 0x500 && vm.cpl() == 0 && !(vm.flags & 0x20000) && vm.r[SP] == 0x8fd8,
              "VM86 CLI exits through #GP on TSS ring-0 stack");
        const uint32_t frame[] = {0,     0x10,  0x100, 0x20202, 0x12340800,
                                  0x200, 0x300, 0x400, 0x500,   0x600};
        for (unsigned n = 0; n < 10; n++)
            check(vm.read(vm.r[SP] + n * 4, 4) == frame[n], "VM86 exception frame word order");
        check(vm.s[ES].selector == 0 && vm.s[DS].selector == 0 && vm.s[FS].selector == 0 &&
                  vm.s[GS].selector == 0,
              "VM86 interrupt clears data selectors");
        vm.step();
        vm.step();
        check(vm.ip == 0x10 && vm.cpl() == 3 && (vm.flags & 0x20000) && vm.r[SP] == 0x12340800 &&
                  vm.s[GS].selector == 0x600 && vm.s[DS].base == 0x4000,
              "IRETD restores complete VM86 context");
        vm.flags |= 0x3000;
        vm.step();
        check(vm.ip == 0x11 && !(vm.flags & IF) && (vm.flags & 0x20000),
              "VM86 IOPL3 CLI executes directly");
        vm.write(0x2000 + 6 * 8, 0x00080600, 4);
        vm.write(0x2004 + 6 * 8, 0x8e00, 4);
        vm.write(0x1011, 0x0d63, 2);
        vm.step();
        check(vm.ip == 0x600 && vm.cpl() == 0 && vm.read(vm.r[SP], 4) == 0x11,
              "ARPL in VM86 raises #UD for the VMM BOP dispatcher");
        CPU cached;
        cached.cr[0] = 0x80000001;
        cached.cr[3] = 0x2000;
        cached.flags = 0x20002;
        cached.mem[0x2000] = 7;
        cached.mem[0x2001] = 0x30;
        cached.mem[0x3000] = 7;
        cached.mem[0x3001] = 0x50;
        cached.mem[0x5000] = 11;
        cached.mem[0x6000] = 22;
        check(cached.read(0, 1) == 11, "TLB maps user page");
        cached.mem[0x3001] = 0x60;
        check(cached.read(0, 1) == 11,
              "PTE writes do not implicitly invalidate cached translation");
        cached.flushTLB();
        check(cached.read(0, 1) == 22, "TLB flush observes changed PTE");
        cached.mem[0x6fff] = 0xaa;
        bool crossFault = false;
        try {
            cached.write(0xfff, 0x1234, 2);
        } catch (const Fault &f) {
            crossFault = f.vector == 14 && f.error == 6 && cached.cr[2] == 0x1000;
        }
        check(crossFault && cached.mem[0x6fff] == 0xaa,
              "cross-page write faults without partially modifying first page");
        cached.mem[0x3000] = 5;
        cached.flushTLB();
        bool writeFault = false;
        try {
            cached.write(0, 1, 1);
        } catch (const Fault &f) {
            writeFault = f.vector == 14 && f.error == 7;
        }
        check(writeFault && cached.mem[0x6000] == 22, "read-only user PTE faults on write");
        CPU smc;
        program(smc, {0xb8, 1, 0, 0x90, 0xf4});
        Jit smcJit(smc, true, 0);
        smcJit.run();
        auto compiles = smcJit.compiledBlocks;
        smc.write(0x800, 7, 1);
        smc.ip = 0;
        smcJit.run();
        check(smcJit.compiledBlocks == compiles,
              "data write on code page does not invalidate unrelated native block");
        smc.write(1, 2, 1);
        smc.ip = 0;
        smcJit.run();
        check(smc.get(AX, 2) == 2 && smcJit.compiledBlocks > compiles,
              "immediate-byte write invalidates translated block");
        CPU protection;
        protection.cr[0] = 0x80000001;
        protection.cr[3] = 0x2000;
        protection.flags = 0x20002;
        protection.mem[0x2000] = 7;
        protection.mem[0x2001] = 0x30;
        protection.mem[0x3000] = 3;
        protection.mem[0x3001] = 0x50;
        bool denied = false;
        try {
            protection.read(0, 1);
        } catch (const Fault &f) {
            denied = f.vector == 14 && f.error == 5;
        }
        check(denied, "VM86 memory uses user paging privilege despite CS RPL zero");
        check(protection.systemRead(0, 1) == 0,
              "descriptor-table accesses use supervisor paging privilege");
        CPU nofpu;
        nofpu.r[SP] = 0x1000;
        nofpu.write(0, 0xe3db, 2);
        nofpu.write(2, 0x00203edd, 4);
        nofpu.write(0x20, 0x5a5a, 2);
        nofpu.step();
        nofpu.step();
        check(nofpu.read(0x20, 2) == 0x5a5a, "absent 387 leaves presence-probe sentinel unchanged");
        nofpu.ip = 0;
        nofpu.cr[0] |= 4;
        unsigned faultVector = 0;
        nofpu.bios = [&](uint8_t n) {
            faultVector = n;
            return true;
        };
        nofpu.step();
        check(faultVector == 7 && nofpu.ip == 0, "CR0.EM sends x87 instruction to #NM handler");
        VGA vga;
        vga.mode(0x12, true);
        vga.gfx[0] = 5;
        vga.gfx[1] = 15;
        vga.write(0xa0000, 255);
        check(vga.planes[0][0] == 255 && vga.planes[1][0] == 0 && vga.planes[2][0] == 255,
              "VGA set/reset planes");
        vga.read(0xa0000);
        vga.gfx[5] = 1;
        vga.write(0xa0001, 0);
        check(vga.planes[0][1] == 255 && vga.planes[2][1] == 255, "VGA write mode 1 uses latches");
        vga.gfx[5] = 8;
        vga.gfx[2] = 5;
        vga.gfx[7] = 15;
        check(vga.read(0xa0000) == 255, "VGA read color compare");
        vga.mode(0x13, true);
        vga.write(0xa0007, 42);
        check(vga.planes[3][1] == 42 && vga.read(0xa0007) == 42, "VGA chain-4 addressing");
        {
            VGA display;
            display.mode(0x13, true);
            display.updateGeometry();
            check(display.width == 320 && display.height == 200,
                  "CRTC geometry preserves standard chained 320x200 mode");
            display.dac[42][0] = 63;
            display.dac[42][1] = display.dac[42][2] = 0;
            display.write(0xa0000 + 199 * 320 + 319, 42);
            check(display.pixel(319, 199) == 0xffff0000,
                  "chained 256-color scanout uses CRTC stride and all four planes");
            display.seq[4] &= ~8;
            display.crtc[9] = 0;
            display.updateGeometry();
            check(display.width == 320 && display.height == 400,
                  "Mode X scanout derives 400 rows independently of BIOS mode number");
            for (unsigned plane = 0; plane < 4; plane++) {
                display.seq[2] = 1u << plane;
                display.write(0xa0000 + 399 * 80, 42);
                check(display.pixel(plane, 399) == 0xffff0000,
                      "unchained Mode X retains 256-color plane-interleaved scanout");
            }
            display.crtc[12] = 0;
            display.crtc[13] = 5;
            display.planes[0][5] = 42;
            check(display.pixel(0, 0) == 0xffff0000, "Mode X display start uses plane offsets");
            display.crtc[9] = 0x80;
            display.updateGeometry();
            check(display.height == 200, "CRTC scan doubling repeats each memory row");
            display.crtc[9] = 0x81;
            display.updateGeometry();
            check(display.height == 100, "maximum scan line and double scan combine");
            display.mode(0x12, true);
            display.updateGeometry();
            check(display.width == 640 && display.height == 480,
                  "CRTC overflow bits retain planar 640x480 geometry");
            display.crtc[0x12] = 0x57;
            display.crtc[7] = 0x40;
            display.updateGeometry();
            check(display.height == 600, "vertical display end includes overflow bit nine");
            display.mode(3, true);
            display.updateGeometry();
            check(!display.graphics, "text mode remains separate from graphics scanout");
        }
        // INT 13h/AH=08 must preserve ES:DI on fixed disks (DOS relies on DI).
        auto tmp = std::filesystem::temp_directory_path() / "pcemu-bios-test.raw";
        {
            std::ofstream f(tmp, std::ios::binary);
            f.seekp(1024 * 1024 - 1);
            f.put(0);
        }
        Machine fixed(tmp.string());
        fixed.cpu.segment(ES, 0x1234);
        fixed.cpu.r[DI] = 0xabcd;
        fixed.cpu.r[AX] = 0x0800;
        fixed.cpu.r[DX] = 0x80;
        fixed.service(0x13);
        check(fixed.cpu.s[ES].selector == 0x1234 && fixed.cpu.r[DI] == 0xabcd,
              "fixed disk geometry preserves ES:DI");
        fixed.out(0x1f6, 0xe0, 1);
        fixed.out(0x1f2, 1, 1);
        fixed.out(0x1f3, 1, 1);
        fixed.out(0x1f4, 0, 1);
        fixed.out(0x1f5, 0, 1);
        fixed.out(0x1f7, 0x30, 1);
        check((fixed.in(0x1f7, 1) & 8) != 0, "ATA write command requests a PIO sector");
        for (unsigned i = 0; i < 256; i++)
            fixed.out(0x1f0, i ^ 0xa55a, 2);
        check(fixed.ata.irq && (fixed.in(0x3f6, 1) & 8) == 0,
              "ATA sector write completes and asserts IRQ");
        fixed.out(0x1f2, 1, 1);
        fixed.out(0x1f3, 1, 1);
        fixed.out(0x1f7, 0x20, 1);
        check(fixed.ata.irq && fixed.in(0x3f6, 1) == 0x58 && fixed.ata.irq,
              "ATA alternate status preserves pending IRQ");
        check(fixed.in(0x1f7, 1) == 0x58 && !fixed.ata.irq, "ATA status acknowledges pending IRQ");
        for (unsigned i = 0; i < 256; i++)
            check(fixed.in(0x1f0, 2) == (i ^ 0xa55a), "ATA PIO word transfer byte order and data");
        check(fixed.in(0x1f2, 1) == 0 && fixed.in(0x1f7, 1) == 0x50,
              "ATA completion exhausts sector count and DRQ");
        fixed.disk->transfer(fixed.cpu, false, 1, 1, 0x2000);
        check(fixed.cpu.read(0x2000, 2) == 0xa55a && fixed.cpu.read(0x21fe, 2) == (255 ^ 0xa55a),
              "BIOS and ATA share backing sectors");
        fixed.out(0x1f7, 0xec, 1);
        uint16_t words[256];
        for (auto &word : words)
            word = fixed.in(0x1f0, 2);
        check(words[49] == 0x200 && words[3] == 16 && words[60] == 2048,
              "ATA IDENTIFY capacity, LBA and physical geometry");
        fixed.out(0x1f6, 0xb0, 1);
        check(fixed.in(0x1f7, 1) == 0, "absent ATA slave is not detected as present");
        fixed.out(0x1f6, 0xe0, 1);
        fixed.out(0x1f7, 0xc8, 1);
        check((fixed.in(0x1f7, 1) & 1) && fixed.in(0x1f1, 1) == 4,
              "unsupported DMA command reports ABRT");
        fixed.out(0x3f6, 4, 1);
        fixed.out(0x3f6, 0, 1);
        check(fixed.in(0x1f7, 1) == 0x50 && fixed.in(0x1f2, 1) == 1 && fixed.in(0x1f3, 1) == 1,
              "ATA software reset restores task-file signature");
        // A tiny boot sector makes hardware and firmware reset paths observable
        // without depending on an installed operating system.
        fixed.disk->file.clear();
        fixed.disk->file.seekp(0);
        fixed.disk->file.put(char(0xf4));
        fixed.disk->file.seekp(4);
        fixed.disk->file.put(char(0x41));
        fixed.disk->file.seekp(510);
        fixed.disk->file.put(char(0x55));
        fixed.disk->file.put(char(0xaa));
        fixed.disk->file.flush();
        for (unsigned resetSource = 0; resetSource < 4; resetSource++) {
            auto before = fixed.cpu.instructions;
            fixed.cpu.cr[0] = 0x10;
            fixed.cpu.flags = 2;
            fixed.cpu.segment(SS, 0x1234);
            fixed.cpu.r[AX] = 0xdeadbeef;
            fixed.cpu.write(0x100000, 0x12345678, 4);
            fixed.pic[0].isr = 2;
            ps2State(fixed).keyboardFull = true;
            ps2State(fixed).scanCodes = {0x1e};
            fixed.vga.mode(0x13, true);
            if (resetSource == 1) {
                fixed.cpu.cr[0] = 0x80000011;
                fixed.cpu.cr[3] = 0x2000;
                fixed.cpu.gdtBase = fixed.cpu.ldtBase = 0xdead0000;
            }
            if (resetSource == 0) {
                fixed.cpu.segment(CS, 0xffff);
                fixed.cpu.ip = 0;
                fixed.cpu.step(); // Reset-vector far jump.
                fixed.cpu.step(); // Firmware bootstrap request.
            } else if (resetSource == 1)
                fixed.out(0x64, 0xfe, 1);
            else if (resetSource == 2)
                fixed.out(0x92, 1, 1);
            else {
                fixed.out(0x64, 0xd1, 1);
                fixed.out(0x60, 2, 1); // Controller output RESET# low.
            }
            check(fixed.resetRequested, "guest reset source requests a firmware restart");
            fixed.poll();
            check(fixed.resets == resetSource + 1 && !fixed.resetRequested &&
                      fixed.cpu.s[CS].selector == 0 && fixed.cpu.s[SS].selector == 0 &&
                      fixed.cpu.ip == 0x7c00 && fixed.cpu.r[SP] == 0x7c00 &&
                      fixed.cpu.r[DX] == 0x80 && fixed.cpu.r[AX] == 0 && fixed.cpu.cr[0] == 0x10 &&
                      fixed.cpu.cr[3] == 0 && fixed.cpu.gdtBase == 0 && fixed.cpu.ldtBase == 0 &&
                      fixed.cpu.instructions >= before && fixed.cpu.read(0x100000, 4) == 0x12345678,
                  "firmware restart preserves RAM and disk while resetting CPU and boot stack");
            check(!ps2State(fixed).keyboardFull && ps2State(fixed).scanCodes.empty() && fixed.pic[0].isr == 0 &&
                      fixed.pic[0].mask == 0xfc && fixed.pic[1].base == 0x70 &&
                      !fixed.vga.graphics && fixed.cpu.read(0x40, 4) == 0xf0001040,
                  "firmware restart restores device state and BIOS vectors");
            fixed.cpu.step();
            check(fixed.cpu.halted, "guest boot sector executes after reset");
            fixed.cpu.halted = false;
        }
        fixed.cpu.segment(CS, 0xf000);
        fixed.cpu.ip = 0x1064; // Saved INT 19h far pointer, not the direct shortcut.
        fixed.cpu.flags = 2 | CF | ZF;
        fixed.cpu.r[SP] = 0x500;
        fixed.cpu.step();
        check(fixed.cpu.ip == 0x7c00 && fixed.cpu.s[CS].selector == 0 &&
                  fixed.cpu.r[SP] == 0x7c00 && fixed.cpu.read(0x7c04, 1) == 0x41,
              "nonreturning INT19 stub does not write caller flags into the new boot sector");
        fixed.vgaRom.assign(16, 0x90);
        fixed.vgaRom[3] = 0xcb; // Tiny option ROM returns to the firmware bootstrap.
        fixed.vgaRom[6] = 0xa0;
        fixed.initializeVga();
        fixed.cpu.write(0xc0006, 0xc1, 1); // Simulated OS patch to ROM shadow.
        fixed.restart();
        check(fixed.cpu.read(0xc0006, 1) == 0xa0 && fixed.cpu.ip == 0x7c00 && !fixed.cpu.halted,
              "firmware restart reloads immutable VGA ROM before executing initialization");
        fixed.floppy = std::make_unique<Disk>(tmp.string(), true);
        fixed.boot(true);
        fixed.service(0x19);
        check(fixed.bootFromFloppy && fixed.cpu.r[DX] == 0,
              "INT19 retains the selected floppy boot device");
        fixed.restart();
        check(fixed.bootFromFloppy && fixed.cpu.r[DX] == 0,
              "hardware restart retains the selected floppy boot device");
        std::filesystem::remove(tmp);
        {
            Machine pulse("", "");
            for (unsigned value = 0xf0; value <= 0xff; value++) {
                pulse.resetRequested = false;
                pulse.out(0x64, value, 1);
                check(pulse.resetRequested == !(value & 1),
                      "8042 pulse commands reset exactly when output bit zero is pulsed");
            }
        }
        for (bool available : {false, true}) {
            Machine chained("", "");
            auto &c = chained.cpu;
            c.ip = 0x100;
            c.r[SP] = 0x800;
            c.flags = 2 | CF | OF | (available ? ZF : 0);
            if (available)
                chained.keys.push_back(0x1c0d);
            // PUSHF; MOV AX,available; OR AX,AX; MOV AH,1;
            // CALL FAR saved BIOS INT16; HLT. Working ZF differs from saved ZF.
            const uint8_t code[] = {
                0x9c, 0xb8, uint8_t(available), 0, 0x09, 0xc0, 0xb4, 1, 0x9a, 0x58, 0x10, 0,
                0xf0, 0xf4};
            for (unsigned i = 0; i < sizeof code; i++)
                c.write(0x100 + i, code[i], 1);
            for (unsigned i = 0; i < 12 && !c.halted; i++)
                c.step();
            check(c.halted && c.r[SP] == 0x800 && bool(c.flags & ZF) == !available &&
                      (c.flags & (CF | OF)) == (CF | OF),
                  "saved BIOS INT16 returns explicit ZF in caller frame despite modified working "
                  "flags");
        }
        for (bool error : {false, true}) {
            Machine chained("", "");
            auto &c = chained.cpu;
            c.ip = 0x100;
            c.r[SP] = 0x800;
            c.flags = 2 | ZF | (error ? 0 : CF);
            c.r[DX] = 0x80;
            // Preserve caller flags, change working CF, then invoke saved INT13.
            const uint8_t code[] = {0x9c,
                                    uint8_t(error ? 0xf9 : 0xf8),
                                    0xb8,
                                    uint8_t(error ? 1 : 0),
                                    uint8_t(error ? 2 : 0),
                                    0x9a,
                                    0x4c,
                                    0x10,
                                    0,
                                    0xf0,
                                    0xf4};
            for (unsigned i = 0; i < sizeof code; i++)
                c.write(0x100 + i, code[i], 1);
            for (unsigned i = 0; i < 12 && !c.halted; i++)
                c.step();
            check(c.halted && c.r[SP] == 0x800 && bool(c.flags & CF) == error && (c.flags & ZF),
                  "saved BIOS INT13 returns explicit CF status rather than working-flag deltas");
        }
        {
            Machine shadow("", "");
            auto &c = shadow.cpu;
            c.segment(CS, 0);
            c.segment(SS, 0);
            c.r[SP] = 0x800;
            c.ip = 0x100;
            c.flags = 2;
            // STI; MOV SP,0x900: the interrupt must see the new stack.
            c.write(0x100, 0xbcfb, 2);
            c.write(0x102, 0x900, 2);
            c.step();
            shadow.timer();
            check(shadow.ticks == 0 && shadow.timerPending,
                  "STI defers pending interrupt through following instruction");
            Jit jit(c, true, 0);
            jit.run();
            shadow.poll();
            check(shadow.ticks == 1 && c.r[SP] == 0x900,
                  "JIT observes STI shadow boundary before delivering IRQ");
            // MOV SS,AX; NOP also protects the subsequent instruction.
            c.ip = 0x110;
            c.r[AX] = 0;
            c.write(0x110, 0x90d08e, 4);
            c.step();
            shadow.timer();
            check(shadow.ticks == 1, "MOV SS inhibits interrupt delivery");
            c.step();
            shadow.poll();
            check(shadow.ticks == 2, "MOV SS shadow expires after one instruction");
        }
        {
            Machine firmware("", "");
            auto &c = firmware.cpu;
            c.segment(CS, 0xf000);
            c.segment(SS, 0);
            c.ip = 0x1020; // Original callable timer vector jumps to extended ROM stub.
            c.r[SP] = 0x8000;
            c.r[AX] = 0x1234;
            c.flags = 2;
            c.push(2, 2);
            c.push(0, 2);
            c.push(0x1234, 2);
            firmware.pic[0].isr = 1;
            unsigned guestEOIs = 0;
            c.output = [&](uint16_t port, uint32_t value, int width) {
                if (port == 0x20 && value == 0x20)
                    guestEOIs++;
                firmware.out(port, value, width);
            };
            for (unsigned n = 0; n < 7; n++)
                c.step();
            check(guestEOIs == 1 && firmware.pic[0].isr == 0 && firmware.ticks == 1 &&
                      c.s[CS].selector == 0 && c.ip == 0x1234 && c.r[AX] == 0x1234,
                  "chained timer firmware acknowledges PIC through observable guest OUT");
        }
        {
            Machine keyboard("", "");
            keyboard.cpu.flags &= ~IF;
            ps2State(keyboard).scanCodes = {0x1e, 0x9e};
            keyboard.poll();
            check((keyboard.in(0x64, 1) & 1) && keyboard.in(0x60, 1) == 0x1e,
                  "keyboard controller latches one scan byte and clears OBF after read");
            check(!(keyboard.in(0x64, 1) & 1) && keyboard.in(0x60, 1) == 0x1e &&
                      ps2State(keyboard).scanCodes.size() == 1,
                  "rereading empty keyboard output does not consume another scan byte");
            keyboard.cpu.instructions += 3999;
            keyboard.poll();
            check(!(keyboard.in(0x64, 1) & 1),
                  "keyboard serial interval prevents immediate next byte");
            keyboard.cpu.instructions++;
            keyboard.poll();
            check((keyboard.in(0x64, 1) & 1) && keyboard.in(0x60, 1) == 0x9e,
                  "keyboard break byte arrives after the serial interval");
            ps2State(keyboard).scanCodes = {0x2e};
            keyboard.pic[0].isr = 2;
            keyboard.cpu.instructions += 4000;
            keyboard.poll();
            check(!(keyboard.in(0x64, 1) & 1) && keyboard.in(0x60, 1) == 0x9e,
                  "scan stream preserves latch while a chained keyboard ISR is active");
            keyboard.pic[0].command(0x20);
            keyboard.poll();
            check(keyboard.in(0x60, 1) == 0x2e, "scan stream resumes after keyboard EOI");
            auto nextByte = [&]() {
                keyboard.cpu.instructions += 4000;
                keyboard.poll();
                check(keyboard.in(0x64, 1) & 1, "keyboard command response becomes available");
                return keyboard.in(0x60, 1);
            };
            keyboard.out(0x64, 0xd1, 1);
            keyboard.out(0x60, 3, 1);
            ps2State(keyboard).scanCodes = {0x30, 0xb0};
            keyboard.out(0x60, 0xed, 1);
            check(keyboard.cpu.a20 && nextByte() == 0xfa && ps2State(keyboard).scanCodes.size() == 2,
                  "LED command acknowledges without consuming key bytes or changing A20");
            keyboard.out(0x60, 7, 1);
            check(nextByte() == 0xfa && ps2State(keyboard).keyboardLEDs == 7,
                  "LED parameter acknowledges and resumes keyboard scanning");
            check(nextByte() == 0x30, "keyboard input resumes after command transaction");
            keyboard.out(0x64, 0x20, 1);
            check(nextByte() == 0x45, "controller command-byte read uses output buffer");
            keyboard.out(0x64, 0xad, 1);
            keyboard.cpu.instructions += 4000;
            keyboard.poll();
            check(!(keyboard.in(0x64, 1) & 1) && ps2State(keyboard).scanCodes.size() == 1,
                  "disabled keyboard interface retains pending input");
            keyboard.out(0x64, 0xae, 1);
            check(nextByte() == 0xb0, "reenabled keyboard delivers retained break byte");
        }
        Machine machine("", "");
        check(!(machine.cpu.cr[0] & 4) && !(machine.cpu.read(0x410, 2) & 2),
              "BIOS advertises no coprocessor without enabling an unhandled #NM trap");
        machine.out(0x70, 0x14, 1);
        check((machine.in(0x71, 1) & 1) == 0, "CMOS equipment reports no absent floppy drive");
        machine.cpu.r[DX] = 0x81;
        machine.cpu.r[AX] = 0x0800;
        machine.service(0x13);
        check((machine.cpu.flags & CF) && machine.cpu.read(0x474, 1) == 0x80,
              "BIOS rejects absent second disk and records disk status");
        machine.cpu.segment(CS, 0);
        machine.cpu.ip = 0x100;
        machine.cpu.write(0x100, 0x10cd, 2);
        machine.cpu.set(AX, 0x0e41, 2);
        machine.cpu.step();
        check(machine.cpu.read(0xb8000, 1) == 'A', "BIOS teletype");
        machine.pic[0].isr = 1;
        machine.out(0x70, 8, 1);
        auto beforeRTC = machine.ticks;
        auto month = machine.in(0x71, 1);
        check(month >= 1 && month <= 0x12 && machine.ticks == beforeRTC && machine.pic[0].isr == 1,
              "CMOS month read leaves timer and PIC service state unchanged");
        machine.pic[0].isr = 0;
        machine.cpu.flags &= ~IF;
        machine.timer();
        check(machine.timerPending && machine.ticks == 0,
              "PIC latches timer IRQ while IF is clear");
        machine.cpu.flags |= IF;
        machine.poll();
        check(!machine.timerPending && !machine.timerInService && machine.ticks == 1 &&
                  machine.cpu.read(0x46c, 4) == 1,
              "pending timer IRQ delivers after STI and firmware acknowledges EOI");
        machine.cpu.cr[0] |= 1;
        machine.key('+');
        check(ps2State(machine).scanCodes == std::deque<uint8_t>({0x2a, 0x0d, 0x8d, 0xaa}),
              "shifted punctuation scan codes");
        machine.cpu.cr[0] &= ~1u;
        ps2State(machine).scanCodes.clear();
        machine.keys.clear();
        machine.key('r');
        check(machine.keys.empty() && ps2State(machine).scanCodes == std::deque<uint8_t>({0x13, 0x93}),
              "host keyboard follows IRQ1 across temporary real-mode transitions");
        ps2State(machine).scanCodes.clear();
        machine.cpu.r[AX] = 0x0500;
        machine.cpu.r[CX] = 0x1e61;
        machine.service(0x16);
        check(machine.keys == std::deque<uint16_t>({0x1e61}) && ps2State(machine).scanCodes.empty(),
              "BIOS insert-key service enqueues a BIOS key rather than physical input");
        machine.keys.clear();
        machine.cpu.write(0x417, 2, 1);
        ps2State(machine).keyboardData = 0x0d;
        machine.cpu.r[AX] = 0x0d;
        machine.service(9, true);
        check(machine.keys == std::deque<uint16_t>({0x0d2b}),
              "firmware translates shifted punctuation from physical keyboard input");
        machine.cpu.write(0x417, 0, 1);
        machine.keys.clear();
        machine.cpu.flags &= ~IF;
        machine.cpu.r[AX] = 0;
        machine.service(0x16);
        check(machine.waitingKey && machine.cpu.halted && (machine.cpu.flags & IF),
              "blocking firmware keyboard read enables IRQ delivery while waiting");
        machine.cpu.halted = false;
        for (unsigned i = 0; i < 17; i++)
            machine.cpu.write(0x200 + i, 15 - i, 1);
        machine.cpu.segment(ES, 0);
        machine.cpu.r[DX] = 0x200;
        machine.cpu.r[AX] = 0x1002;
        machine.service(0x10);
        check(machine.vga.attr[1] == 14 && machine.vga.attr[15] == 0, "BIOS bulk palette service");
        std::cout << "Passed " << checks << " checks\n";
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << " (after " << checks << " checks)\n";
        return 1;
    }
}
