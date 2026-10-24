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
        unsigned trial = 0;
        auto run = [&](std::vector<uint8_t> code, unsigned mode) {
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
                ref.r[reg] = native.r[reg] = value;
            }
            ref.flags = native.flags = 2 | (rng() & 0x8d5);
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
                               0x32, 0x38, 0x3a, 0x84})
                for (unsigned dst = 0; dst < 8; dst++)
                    for (unsigned src = 0; src < 8; src++)
                        run({op, uint8_t(0xc0 | (src << 3) | dst)}, mode);
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
        check(jit.cacheRecycles > 0, "byte blocks remain correct across repeated arena recycling");
        std::cout << "Passed " << checks << " byte JIT checks\n";
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << " after " << checks << " checks\n";
        return 1;
    }
}
