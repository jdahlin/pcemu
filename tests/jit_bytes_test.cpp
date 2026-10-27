#include "cpu.hpp"
#include "jit.hpp"
#include <algorithm>
#include <iostream>
#include <random>
using namespace pc;
static unsigned checks = 0;
static void check(bool ok, const char *message) {
    ++checks;
    if (!ok)
        throw std::runtime_error(message);
}
int main() {
    try {
        CPU ref(65536), native(65536);
        Jit jit(native, true, 0, 65536);
        if (!jit.enabled()) {
            std::cout << "Byte JIT tests require Apple ARM64\n";
            return 0;
        }
        std::mt19937 rng(39183);
        auto compareBlock = [&](const std::vector<uint8_t> &code, uint32_t value,
                                uint32_t flags) {
            for (auto c : {&ref, &native}) {
                c->s[CS].big = true;
                c->ip = c->instructions = 0;
                c->flags = flags;
                c->r[AX] = value;
                for (unsigned n = 0; n < code.size(); ++n)
                    c->write(n, code[n], 1);
            }
            ref.step();
            ref.step();
            check(jit.run() == 2 && native.ip == ref.ip && native.flags == ref.flags &&
                      native.r[AX] == ref.r[AX],
                  "native ALU flags and taken/untaken Jcc match interpreter");
        };
        // Every flag combination and predicate, with distinct branch targets,
        // also covers blocks which consume flags without producing any.
        for (unsigned cc = 0; cc < 16; ++cc)
            for (unsigned pattern = 0; pattern < 32; ++pattern) {
                uint32_t flags = 0x202 | ((pattern & 1) ? OF : 0) |
                                 ((pattern & 2) ? CF : 0) | ((pattern & 4) ? ZF : 0) |
                                 ((pattern & 8) ? SF : 0) | ((pattern & 16) ? PF : 0);
                compareBlock({0xb0, 0x23, uint8_t(0x70 | cc), 0x10}, rng(), flags);
            }
        for (unsigned width : {1u, 2u, 4u})
            for (unsigned operation : {0u, 1u, 4u, 5u, 6u, 7u, 8u})
                for (unsigned cc = 0; cc < 16; ++cc)
                    for (uint32_t value : {0u, 1u, 0x10u, 0xffu, 0x8000u, 0xffffu,
                                           0x80000000u, 0xffffffffu}) {
                        std::vector<uint32_t> operands{rng()};
                        if (operation == 0 || operation == 5 || operation == 7)
                            operands = {0, 1, 0xf, 0x10, 0x7f, 0x80, 0xff, 0x7fff,
                                        0x8000, 0xffff, 0x7fffffff, 0x80000000, 0xffffffff};
                        for (auto operand : operands) {
                            std::vector<uint8_t> code;
                            if (width == 2)
                                code.push_back(0x66);
                            code.push_back(operation == 8 ? (width == 1 ? 0xa8 : 0xa9)
                                                          : operation * 8 + (width == 1 ? 4 : 5));
                            for (unsigned n = 0; n < width; ++n)
                                code.push_back(operand >> (n * 8));
                            code.insert(code.end(), {uint8_t(0x70 | cc), 0x10});
                            compareBlock(code, value, 0x244ed7);
                        }
                    }
        unsigned trial = 0;
        auto run = [&](std::vector<uint8_t> code, unsigned mode, int carry = -1, int byteValue = -1) {
            bool big = mode & 1;
            if (mode & 2)
                code.insert(code.begin(), 0x66); // Byte operands ignore operand-size overrides.
            code.push_back(0x70 | (trial & 15));
            code.push_back(0);
            for (auto c : {&ref, &native}) {
                c->s[CS].big = big;
                c->ip = 0;
                c->instructions = 0;
                c->halted = false;
                for (unsigned n = 0; n < code.size(); n++)
                    c->write(n, code[n], 1);
            }
            const uint8_t edges[] = {0, 1, 0x7f, 0x80, 0xfe, 0xff};
            for (unsigned reg = 0; reg < 8; reg++) {
                uint32_t value = rng();
                if (trial % 3 == 0)
                    value = (value & 0xffff0000u) | (uint32_t(edges[(trial / 3 + reg) % 6]) << 8) |
                            edges[(trial + reg) % 6];
                if (byteValue >= 0)
                    value = (value & 0xffff0000u) | (unsigned(byteValue) << 8) | unsigned(byteValue);
                ref.r[reg] = native.r[reg] = value;
            }
            ref.flags = native.flags = 2 | (rng() & 0x8d5);
            if (carry >= 0)
                ref.flags = native.flags = (ref.flags & ~CF) | unsigned(carry);
            ref.step();
            ref.step();
            auto n = jit.run();
            check(n == 2 && native.instructions == 2 && native.ip == ref.ip &&
                      native.flags == ref.flags,
                  "byte lowering preserves exact flags, branch and instruction count");
            for (unsigned reg = 0; reg < 8; reg++)
                check(ref.r[reg] == native.r[reg],
                      "byte lowering preserves every GPR bit outside its operand");
            ++trial;
        };
        for (unsigned mode = 0; mode < 4; mode++)
            for (uint8_t op : {0x88, 0x8a, 0x00, 0x02, 0x08, 0x0a, 0x20, 0x22, 0x28, 0x2a, 0x30,
                               0x32, 0x38, 0x3a, 0x84, 0x86})
                for (unsigned dst = 0; dst < 8; dst++)
                    for (unsigned src = 0; src < 8; src++)
                        run({op, uint8_t(0xc0 | (src << 3) | dst)}, mode);
        for (unsigned mode = 0; mode < 4; ++mode) {
            for (unsigned dst = 0; dst < 8; ++dst)
                for (unsigned src = 0; src < 8; ++src)
                    for (unsigned repeat = 0; repeat < 16; ++repeat)
                        run({0x87, uint8_t(0xc0 | (src << 3) | dst)}, mode);
            for (unsigned reg = 0; reg < 8; ++reg)
                for (unsigned repeat = 0; repeat < 16; ++repeat)
                    run({uint8_t(0x90 | reg)}, mode);
        }
        for (unsigned mode = 0; mode < 4; ++mode)
            for (unsigned decrement = 0; decrement < 2; ++decrement)
                for (unsigned reg = 0; reg < 8; ++reg)
                    for (unsigned value = 0; value < 256; ++value)
                        for (unsigned carry = 0; carry < 2; ++carry)
                            run({0xfe, uint8_t(0xc0 | (decrement << 3) | reg)},
                                mode, carry, value);
        for (unsigned dst = 0; dst < 8; dst++)
            for (unsigned value = 0; value < 256; value++)
                run({uint8_t(0xb0 | dst), uint8_t(value)}, value & 3);
        for (uint8_t op : {0x04, 0x0c, 0x24, 0x2c, 0x34, 0x3c})
            for (unsigned value = 0; value < 256; value++)
                run({op, uint8_t(value)}, value & 3);
        for (unsigned group : {0u, 1u, 4u, 5u, 6u, 7u})
            for (unsigned dst = 0; dst < 8; dst++)
                for (unsigned value = 0; value < 256; value++)
                    run({uint8_t(trial & 1 ? 0x80 : 0x82), uint8_t(0xc0 | (group << 3) | dst),
                         uint8_t(value)},
                        (value + dst + group) & 3);
        for (bool store : {false, true})
            for (unsigned kind = 0; kind < 4; kind++) {
                if (kind == 1 && !store)
                    continue;
                CPU expected(1024 * 1024), actual(1024 * 1024);
                const uint8_t code[] = {
                    0xb8, 0xff, 0xff, 0xbb, 0xaa, 0x80, 0xc4, 1,    uint8_t(store ? 0x88 : 0x8a),
                    0x25, 0,    0x90, 0,    0,    0xbb, 0xef, 0xbe, 0xad,
                    0xde, 0xf4};
                for (auto c : {&expected, &actual}) {
                    std::copy(std::begin(code), std::end(code), c->mem.begin());
                    c->s[CS].big = true;
                    c->write(0x80000, 0x81007, 4);
                    c->write(0x81000, 7, 4);
                    if (kind == 1 || kind == 2)
                        c->write(0x81024, kind == 1 ? 0x9045 : 0x9043, 4);
                    c->cr[3] = 0x80000;
                    c->cr[0] = kind == 3 ? 0x11 : 0x80000011;
                    if (kind == 1 || kind == 2)
                        c->physical(0x9000);
                    c->s[CS].selector = 3;
                    c->s[DS].unusable = kind == 3;
                    c->cr[2] = 0x13579;
                }
                struct Stop {};
                unsigned seen = 0;
                auto observe = [&](CPU &c, const Fault &fault) {
                    check(fault.vector == (kind == 3 ? 13 : 14) &&
                              fault.error ==
                                  (kind == 3 ? 0u : (kind ? 1u : 0u) | 4u | (store ? 2u : 0u)) &&
                              c.ip == 8 && c.r[AX] == 0xaabb00ff && c.r[BX] == 0 &&
                              c.cr[2] == (kind == 3 ? 0x13579u : 0x9000u) &&
                              (c.flags & (CF | ZF | PF | AF | SF | OF)) == (CF | ZF | PF | AF),
                          "faulting high-byte memory access commits prior byte arithmetic exactly");
                    seen++;
                    throw Stop{};
                };
                expected.observeFault = [&](const Fault &f) { observe(expected, f); };
                actual.observeFault = [&](const Fault &f) { observe(actual, f); };
                try {
                    expected.step();
                    expected.step();
                    expected.step();
                } catch (const Stop &) {
                }
                Jit guarded(actual, true, 0);
                try {
                    guarded.run();
                } catch (const Stop &) {
                }
                check(
                    seen == 2 && actual.instructions == 3 && guarded.nativeInstructions == 2 &&
                        actual.flags == expected.flags && actual.mem == expected.mem,
                    "byte memory fault replay preserves partial registers, flags and permissions");
            }
        for (bool store : {false, true}) {
            CPU mmio(1024 * 1024);
            mmio.s[CS].big = true;
            mmio.s[DS].limit = 0xffffffff; // Flat synthetic MMIO address space.
            const uint8_t code[] = {
                0xb8, 0xdd, 0xcc, 0xbb, 0xaa, 0x80, 0xc4, 1, uint8_t(store ? 0x88 : 0x8a),
                0x25, 0,    0,    0x0a, 0,    0xf4};
            std::copy(std::begin(code), std::end(code), mmio.mem.begin());
            unsigned accesses = 0;
            mmio.videoRead = [&](uint32_t) {
                accesses++;
                return uint8_t(0x12);
            };
            mmio.videoWrite = [&](uint32_t, uint8_t value) {
                accesses++;
                check(value == 0xcd, "MMIO high-byte store supplies only AH");
            };
            Jit guarded(mmio, true, 0);
            check(guarded.run() == 3 && guarded.nativeInstructions == 2 && accesses == 1 &&
                      mmio.r[AX] == (store ? 0xaabbcdddu : 0xaabb12ddu),
                  "byte MMIO fallback executes exactly once and preserves unrelated register bits");
        }
        {
            CPU smc(65536);
            smc.s[CS].big = true;
            const uint8_t code[] = {0xb8, 0, 0x7f, 0xbb, 0xaa, 0x88, 0x25, 12,  0,
                                    0,    0, 0xbb, 0x11, 0x11, 0x11, 0x11, 0xf4};
            std::copy(std::begin(code), std::end(code), smc.mem.begin());
            Jit changed(smc, true, 0);
            check(changed.run() == 2 && smc.ip == 11 && smc.r[BX] == 0,
                  "high-byte code store exits before stale future IR executes");
            check(changed.run() == 1 && smc.r[BX] == 0x1111117f,
                  "byte self-modification changes exactly one immediate byte");
        }
        {
            CPU alias(65536);
            alias.s[CS].big = true;
            const uint8_t code[] = {0xb8, 5, 0x50, 0, 0, 0x8a, 0x20, 0x88, 0x20, 0x8a, 0x38, 0xf4};
            std::copy(std::begin(code), std::end(code), alias.mem.begin());
            alias.mem[0x5005] = 0x60;
            Jit aliased(alias, true, 0);
            check(aliased.run() == 4 && alias.r[AX] == 0x6005 && alias.r[BX] == 0x6000 &&
                      alias.mem[0x6005] == 0x60 && alias.nativeMemoryCalls == 0,
                  "AH loads use the old EAX address and later byte accesses use its updated value");
        }
        for (bool code32 : {false, true})
            for (bool operandOverride : {false, true})
                for (bool addressOverride : {false, true})
                    for (unsigned opcode : {0xe2u, 0xe3u})
                        for (uint32_t origin : {0u, 0xfff0u})
                            for (unsigned displacement : {0u, 127u, 128u, 255u})
                                for (uint32_t count : {0u, 1u, 2u, 0xffffu, 0x10000u,
                                                       0x10001u, 0xffffffffu})
                                    for (unsigned flags : {2u, 0x8d7u}) {
                                        CPU ref(1024 * 1024), native(1024 * 1024);
                                        std::vector<uint8_t> code = {0x83, 0xc3, 1};
                                        if (operandOverride)
                                            code.push_back(0x66);
                                        if (addressOverride)
                                            code.push_back(0x67);
                                        code.push_back(opcode);
                                        code.push_back(displacement);
                                        for (auto c : {&ref, &native}) {
                                            c->s[CS].big = code32;
                                            c->ip = origin;
                                            c->r[CX] = count;
                                            c->r[BX] = 0x7fffffff;
                                            c->flags = flags;
                                            std::copy(code.begin(), code.end(), c->mem.begin() + origin);
                                        }
                                        ref.step();
                                        ref.step();
                                        Jit loop(native, true, 0);
                                        check(loop.run() == 2 && loop.nativeInstructions == 2,
                                              "LOOP/JCXZ execute as native block terminators");
                                        check(native.ip == ref.ip && native.flags == ref.flags &&
                                                  native.instructions == ref.instructions,
                                              "LOOP/JCXZ preserve arithmetic flags and branch IP width");
                                        for (unsigned reg = 0; reg < 8; ++reg)
                                            check(native.r[reg] == ref.r[reg],
                                                  "LOOP/JCXZ preserve independent CX/ECX counter width");
                                    }
        for (bool code32 : {false, true})
            for (bool operandOverride : {false, true})
                for (unsigned extended : {0xb6u, 0xb7u, 0xbeu, 0xbfu})
                    for (unsigned dst = 0; dst < 8; ++dst)
                        for (unsigned src = 0; src < 8; ++src)
                            for (uint32_t value : {0u, 0x7f7fu, 0x8080u, 0xffffu,
                                                   0x7fffu, 0x8000u, 0x12345678u})
                                for (unsigned cc = 0; cc < 16; ++cc) {
                                    std::vector<uint8_t> code = {0x83, 0xc3, 1};
                                    if (operandOverride)
                                        code.push_back(0x66);
                                    code.insert(code.end(), {0x0f, uint8_t(extended),
                                                            uint8_t(0xc0 | (dst << 3) | src),
                                                            uint8_t(0x70 | cc), 0x10});
                                    for (auto c : {&ref, &native}) {
                                        c->s[CS].big = code32;
                                        c->ip = 0;
                                        c->instructions = 0;
                                        c->flags = 0x8d7;
                                        for (unsigned reg = 0; reg < 8; ++reg)
                                            c->r[reg] = value | 0xabcd0000u;
                                        for (unsigned n = 0; n < code.size(); ++n)
                                            c->write(n, code[n], 1);
                                    }
                                    ref.step();
                                    ref.step();
                                    ref.step();
                                    check(jit.run() == 3 && native.ip == ref.ip &&
                                              native.flags == ref.flags,
                                          "MOVZX/MOVSX preserve flags used by subsequent Jcc");
                                    for (unsigned reg = 0; reg < 8; ++reg)
                                        check(native.r[reg] == ref.r[reg],
                                              "MOVZX/MOVSX preserve aliases, sign bits and upper words");
                                }
        {
            CPU expected(65536), actual(65536);
            const uint8_t code[] = {0xb8, 0xff, 0xff, 0xff, 0x7f, 0xbb, 0xab, 0x56, 0x34, 0x12,
                                    0x83, 0xc0, 1, 0x86, 0xdc, 0x64, 0x8b, 0x0d, 0, 0, 0, 0};
            struct Stop {};
            unsigned faults = 0;
            for (auto c : {&expected, &actual}) {
                c->cr[0] = 0x11;
                c->s[CS].big = true;
                c->s[FS].unusable = true;
                std::copy(std::begin(code), std::end(code), c->mem.begin());
                c->observeFault = [&, c](const Fault &f) {
                    ++faults;
                    check(f.vector == 13 && c->ip == 15 && c->r[AX] == 0x8000ab00 &&
                              c->r[BX] == 0x12345600 && (c->flags & (OF | SF)) == (OF | SF),
                          "fault after XCHG publishes both swapped GPRs and prior arithmetic flags");
                    throw Stop{};
                };
            }
            try {
                for (unsigned n = 0; n < 5; ++n)
                    expected.step();
            } catch (const Stop &) {
            }
            Jit exchanged(actual, true, 0);
            try {
                exchanged.run();
            } catch (const Stop &) {
            }
            check(faults == 2 && exchanged.nativeInstructions == 4 &&
                      actual.instructions == expected.instructions && actual.flags == expected.flags,
                  "XCHG effects and following fault replay exactly match interpreter state");
        }
        for (bool code32 : {false, true})
            for (bool overrideWidth : {false, true})
                for (bool modrm : {false, true})
                    for (unsigned reg = 0; reg < 8; ++reg)
                        for (unsigned decrement = 0; decrement < 2; ++decrement)
                            for (uint32_t value : {0u, 1u, 0x7fffu, 0x8000u, 0xffffu,
                                                   0x7fffffffu, 0x80000000u, 0xffffffffu})
                                for (unsigned carry = 0; carry < 2; ++carry)
                                    for (unsigned cc = 0; cc < 16; ++cc) {
                                        std::vector<uint8_t> code;
                                        if (overrideWidth)
                                            code.push_back(0x66);
                                        if (modrm)
                                            code.insert(code.end(), {0xff, uint8_t(0xc0 | decrement << 3 | reg)});
                                        else
                                            code.push_back(0x40 | decrement << 3 | reg);
                                        code.insert(code.end(), {uint8_t(0x70 | cc), 0x10});
                                        for (auto c : {&ref, &native}) {
                                            c->s[CS].big = code32;
                                            c->ip = c->instructions = 0;
                                            c->flags = 0x8d6 | carry;
                                            for (unsigned n = 0; n < 8; ++n)
                                                c->r[n] = value;
                                            for (unsigned n = 0; n < code.size(); ++n)
                                                c->write(n, code[n], 1);
                                        }
                                        ref.step();
                                        ref.step();
                                        check(jit.run() == 2 && native.flags == ref.flags && native.ip == ref.ip,
                                              "INC/DEC preserve CF and compute all remaining flags for Jcc");
                                        for (unsigned n = 0; n < 8; ++n)
                                            check(native.r[n] == ref.r[n], "INC/DEC preserve upper words and other registers");
                                    }
        for (bool big : {false, true})
            for (bool subtract : {false, true})
                for (bool memoryCompare : {false, true})
                    for (bool overwriteFlags : {false, true})
                        for (uint32_t value : {0u, 0xffffu, 0xffffffffu, 0x7fffffffu})
                            for (unsigned cc = 0; cc < 16; ++cc) {
                                std::vector<uint8_t> code;
                                unsigned instructions = 0;
                                if (memoryCompare) {
                                    code = {0x3b, uint8_t(big ? 5 : 6), 0, 0x40};
                                    if (big)
                                        code.insert(code.end(), {0, 0});
                                } else
                                    code = {0x83, uint8_t(subtract ? 0xe8 : 0xc0), 1};
                                ++instructions;
                                code.insert(code.end(), {0x43, 0x4a});
                                instructions += 2;
                                if (overwriteFlags) {
                                    code.insert(code.end(), {0x83, 0xc6, 1});
                                    ++instructions;
                                }
                                code.insert(code.end(), {0xfe, 0xc4, uint8_t(0x70 | cc), 0x10});
                                instructions += 2;
                                for (auto c : {&ref, &native}) {
                                    c->s[CS].big = big;
                                    c->ip = c->instructions = 0;
                                    c->flags = 0x8d7;
                                    for (unsigned reg = 0; reg < 8; ++reg)
                                        c->r[reg] = value;
                                    for (unsigned n = 0; n < code.size(); ++n)
                                        c->write(n, code[n], 1);
                                    c->write(0x4000, 0x80, 4);
                                }
                                for (unsigned n = 0; n < instructions; ++n)
                                    ref.step();
                                check(jit.run() == instructions && native.flags == ref.flags && native.ip == ref.ip,
                                      "INC/DEC chains consume pending ALU/CMP carry and honor later full flag writes");
                                for (unsigned reg = 0; reg < 8; ++reg)
                                    check(native.r[reg] == ref.r[reg], "INC/DEC chains produce reference registers");
                            }
        for (unsigned carry : {0u, 1u}) {
            CPU expected(65536), actual(65536);
            std::vector<uint8_t> code = {0xb8, 0xff, 0xff, 0xff, uint8_t(carry ? 0xff : 0x7f),
                                         0xbb, 0xff, 0xff, 0xff, 0xff, 0x83, 0xc0, 1, 0x43,
                                         0x64, 0x8b, 0x0d, 0, 0, 0, 0};
            struct Stop {};
            unsigned faults = 0;
            for (auto c : {&expected, &actual}) {
                c->cr[0] = 0x11;
                c->s[CS].big = true;
                c->s[FS].unusable = true;
                std::copy(code.begin(), code.end(), c->mem.begin());
                c->observeFault = [&, c](const Fault &f) {
                    ++faults;
                    check(f.vector == 13 && c->ip == 14 && c->r[BX] == 0 &&
                              (c->flags & CF) == carry && (c->flags & ZF),
                          "fault after INC publishes preserved ADD carry and new zero flag");
                    throw Stop{};
                };
            }
            try {
                for (unsigned n = 0; n < 5; ++n)
                    expected.step();
            } catch (const Stop &) {
            }
            Jit incremented(actual, true, 0);
            try {
                incremented.run();
            } catch (const Stop &) {
            }
            check(faults == 2 && incremented.nativeInstructions == 4 &&
                      actual.instructions == expected.instructions && actual.flags == expected.flags,
                  "INC carry and flags remain exact through later memory-fault replay");
        }
        unsigned multiplyTrial = 0;
        const std::pair<uint32_t, uint32_t> multiplyEdges[] = {
            {0, 0xffffffff}, {1, 0x80000000}, {0xffffffff, 0xffffffff},
            {0x80000000, 0xffffffff}, {0x7fffffff, 2}, {0xffff8000, 0xffffffff},
            {0x7fff, 2}, {0x8000, 0xffff}, {0xffff, 0x80}, {0x12345678, 0x7f},
            {0x10000, 0x10000}, {0x7fffffff, 0x80000000}, {0x80000000, 0x80000000},
            {46340, 46340}, {46341, 46341}};
        for (bool code32 : {false, true})
            for (bool overrideWidth : {false, true})
                for (unsigned form = 0; form < 3; ++form)
                    for (unsigned dst = 0; dst < 8; ++dst)
                        for (unsigned src = 0; src < 8; ++src)
                            for (auto [a, b] : multiplyEdges)
                                for (uint32_t flags : {0x244602u, 0x244ed7u}) {
                                    unsigned width = code32 != overrideWidth ? 4 : 2;
                                    std::vector<uint8_t> code;
                                    if (overrideWidth)
                                        code.push_back(0x66);
                                    if (!form)
                                        code.insert(code.end(), {0x0f, 0xaf});
                                    else
                                        code.push_back(form == 1 ? 0x69 : 0x6b);
                                    code.push_back(0xc0 | (dst << 3) | src);
                                    if (form)
                                        for (unsigned n = 0; n < (form == 2 ? 1 : width); ++n)
                                            code.push_back(b >> (n * 8));
                                    code.insert(code.end(), {uint8_t(0x70 | (multiplyTrial++ & 15)), 0x10});
                                    for (auto c : {&ref, &native}) {
                                        c->s[CS].big = code32;
                                        c->ip = c->instructions = 0;
                                        c->flags = flags;
                                        for (unsigned n = 0; n < 8; ++n)
                                            c->r[n] = 0xabcdef00 + n;
                                        c->r[dst] = a;
                                        c->r[src] = form ? a : b;
                                        for (unsigned n = 0; n < code.size(); ++n)
                                            c->write(n, code[n], 1);
                                    }
                                    ref.step();
                                    ref.step();
                                    check(jit.run() == 2 && native.flags == ref.flags &&
                                              native.ip == ref.ip && native.instructions == ref.instructions,
                                          "IMUL forms preserve exact flags, predicate and instruction count");
                                    for (unsigned n = 0; n < 8; ++n)
                                        check(native.r[n] == ref.r[n],
                                              "IMUL preserves aliases, source registers and upper words");
                                }
        // IMUL preserves flags from a preceding producer except CF/OF; an
        // INC consumes its new carry, and a later fault must see all effects.
        for (bool logical : {false, true})
            for (bool overflow : {false, true})
                for (unsigned cc = 0; cc < 16; ++cc) {
                    std::vector<uint8_t> code = logical ? std::vector<uint8_t>{0x31, 0xdb}
                                                        : std::vector<uint8_t>{0x83, 0xfb, 1};
                    code.insert(code.end(), {0x0f, 0xaf, 0xc1, uint8_t(0x70 | cc), 0x10});
                    for (auto c : {&ref, &native}) {
                        c->s[CS].big = true;
                        c->ip = c->instructions = 0;
                        c->flags = 2;
                        c->r[AX] = overflow ? 0x80000000 : 3;
                        c->r[CX] = 0xffffffff;
                        c->r[BX] = 0;
                        for (unsigned n = 0; n < code.size(); ++n)
                            c->write(n, code[n], 1);
                    }
                    for (unsigned n = 0; n < 3; ++n)
                        ref.step();
                    check(jit.run() == 3 && native.flags == ref.flags && native.ip == ref.ip,
                          "IMUL merges pending logical/arithmetic flags with CF/OF before Jcc");
                }
        for (bool overflow : {false, true}) {
            CPU expected(65536), actual(65536);
            std::vector<uint8_t> code = {0x83, 0xfb, 1, 0x0f, 0xaf, 0xc1, 0x42,
                                         0x64, 0x8b, 0x35, 0, 0, 0, 0};
            struct Stop {};
            unsigned faults = 0;
            for (auto c : {&expected, &actual}) {
                c->cr[0] = 0x11;
                c->s[CS].big = true;
                c->s[FS].unusable = true;
                c->r[AX] = overflow ? 0x80000000 : 3;
                c->r[CX] = 0xffffffff;
                c->r[BX] = 0;
                c->r[DX] = 0xffffffff;
                std::copy(code.begin(), code.end(), c->mem.begin());
                c->observeFault = [&, c](const Fault &f) {
                    ++faults;
                    check(f.vector == 13 && c->ip == 7 && c->r[DX] == 0 &&
                              bool(c->flags & CF) == overflow && (c->flags & ZF),
                          "IMUL carry survives INC and a later memory fault");
                    throw Stop{};
                };
            }
            try {
                for (unsigned n = 0; n < 4; ++n)
                    expected.step();
            } catch (const Stop &) {
            }
            Jit multiplied(actual, true, 0);
            try {
                multiplied.run();
            } catch (const Stop &) {
            }
            check(faults == 2 && multiplied.nativeInstructions == 3 &&
                      actual.instructions == expected.instructions && actual.flags == expected.flags &&
                      actual.r[AX] == expected.r[AX],
                  "multiply flag dependencies and fault restart exactly match interpreter");
        }
        unsigned shiftTrial = 0;
        for (bool code32 : {false, true})
            for (bool overrideWidth : {false, true})
                for (bool byteOperand : {false, true})
                    for (unsigned operation : {4u, 5u, 6u, 7u})
                        for (unsigned reg = 0; reg < 8; ++reg)
                            for (unsigned count : {0u, 1u, 2u, 7u, 8u, 9u, 15u, 16u,
                                                   17u, 31u, 32u, 33u, 63u, 127u, 128u, 255u})
                                for (bool implicit : {false, true}) {
                                    if (implicit && count != 1)
                                        continue;
                                    for (uint32_t value : {0u, 1u, 0x7fu, 0x80u, 0xffu, 0x7fffu,
                                                           0x8000u, 0xffffu, 0x7fffffffu,
                                                           0x80000000u, 0xffffffffu})
                                        for (uint32_t flags : {0x244602u, 0x244ed7u}) {
                                            unsigned width = byteOperand ? 1 : code32 != overrideWidth ? 4 : 2;
                                            std::vector<uint8_t> code;
                                            if (overrideWidth)
                                                code.push_back(0x66);
                                            code.push_back((implicit ? 0xd0 : 0xc0) + !byteOperand);
                                            code.push_back(0xc0 | (operation << 3) | reg);
                                            if (!implicit)
                                                code.push_back(count);
                                            code.insert(code.end(), {uint8_t(0x70 | (shiftTrial++ & 15)), 0x10});
                                            for (auto c : {&ref, &native}) {
                                                c->s[CS].big = code32;
                                                c->ip = c->instructions = 0;
                                                c->flags = flags;
                                                for (unsigned n = 0; n < 8; ++n)
                                                    c->r[n] = 0xaabbcc00 + n;
                                                c->set(reg, value, width);
                                                for (unsigned n = 0; n < code.size(); ++n)
                                                    c->write(n, code[n], 1);
                                            }
                                            ref.step();
                                            ref.step();
                                            check(jit.run() == 2 && native.flags == ref.flags &&
                                                      native.ip == ref.ip && native.instructions == ref.instructions,
                                                  "immediate shifts match result flags, count masking and Jcc");
                                            for (unsigned n = 0; n < 8; ++n)
                                                check(native.r[n] == ref.r[n],
                                                      "shifts preserve byte aliases, upper words and other GPRs");
                                        }
                                }
        for (unsigned operation : {4u, 5u, 7u})
            for (unsigned count : {0u, 1u, 2u, 8u, 31u, 32u, 255u})
                for (unsigned cc = 0; cc < 16; ++cc) {
                    std::vector<uint8_t> code = {0x83, 0xc3, 1, 0xc1,
                                                 uint8_t(0xc0 | operation << 3), uint8_t(count),
                                                 uint8_t(0x70 | cc), 0x10};
                    for (auto c : {&ref, &native}) {
                        c->s[CS].big = true;
                        c->ip = c->instructions = 0;
                        c->flags = 2;
                        c->r[AX] = 0x80000001;
                        c->r[BX] = 0x7fffffff;
                        for (unsigned n = 0; n < code.size(); ++n)
                            c->write(n, code[n], 1);
                    }
                    for (unsigned n = 0; n < 3; ++n)
                        ref.step();
                    check(jit.run() == 3 && native.flags == ref.flags && native.ip == ref.ip,
                          "shifts preserve pending ADD AF/OF, including zero counts");
                }
        for (unsigned operation : {4u, 5u, 7u})
            for (unsigned count : {0u, 1u, 2u, 31u, 32u})
                for (bool carryConsumer : {false, true}) {
                    CPU expected(65536), actual(65536);
                    std::vector<uint8_t> code = {0x83, 0xc3, 1, 0xc1,
                                                 uint8_t(0xc0 | operation << 3), uint8_t(count)};
                    if (carryConsumer)
                        code.push_back(0x42);
                    auto faultIP = code.size();
                    code.insert(code.end(), {0x64, 0x8b, 0x3d, 0, 0, 0, 0});
                    struct Stop {};
                    unsigned faults = 0;
                    for (auto c : {&expected, &actual}) {
                        c->cr[0] = 0x11;
                        c->s[CS].big = true;
                        c->s[FS].unusable = true;
                        c->r[AX] = 0x80000001;
                        c->r[BX] = 0x7fffffff;
                        c->r[DX] = 0xffffffff;
                        std::copy(code.begin(), code.end(), c->mem.begin());
                        c->observeFault = [&, c](const Fault &f) {
                            ++faults;
                            check(f.vector == 13 && c->ip == faultIP,
                                  "fault following native shift restarts at its own instruction");
                            throw Stop{};
                        };
                    }
                    try {
                        for (unsigned n = 0; n < 3 + unsigned(carryConsumer); ++n)
                            expected.step();
                    } catch (const Stop &) {
                    }
                    Jit shifted(actual, true, 0);
                    try {
                        shifted.run();
                    } catch (const Stop &) {
                    }
                    check(faults == 2 && shifted.nativeInstructions == 2 + unsigned(carryConsumer) &&
                              actual.instructions == expected.instructions && actual.flags == expected.flags,
                          "shift flags and optional carry-consuming INC survive fault replay");
                    for (unsigned n = 0; n < 8; ++n)
                        check(actual.r[n] == expected.r[n], "fault publishes shifted and preceding registers");
                }
        check(jit.cacheRecycles > 0, "byte blocks remain correct across repeated arena recycling");
        std::cout << "Passed " << checks << " byte JIT checks\n";
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << " after " << checks << " checks\n";
        return 1;
    }
}
