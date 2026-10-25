#include "cpu.hpp"
#include "jit.hpp"
#include <algorithm>
#include <iostream>
#include <random>
using namespace pc;
static unsigned checks;
static void check(bool ok, const char *message) {
    ++checks;
    if (!ok)
        throw std::runtime_error(message);
}
static void equal(const CPU &a, const CPU &b) {
    check(a.ip == b.ip && a.flags == b.flags && a.instructions == b.instructions,
          "combined block preserves flags, IP and guest instruction count");
    for (unsigned n = 0; n < 8; ++n)
        check(a.r[n] == b.r[n], "combined block preserves all guest GPRs");
}
int main() {
    try {
        CPU reference(65536), actual(65536);
        Jit jit(actual, true, 0, 65536);
        if (!jit.enabled()) {
            std::cout << "Flag-liveness tests require Apple ARM64\n";
            return 0;
        }
        // Runtime shifts capture CL before an aliased destination changes.
        // Include all count bytes, high-byte destinations, overshifts and
        // pending ADD flags that must survive a masked-zero count.
        for (bool code32 : {false, true})
            for (unsigned width : {1u, 2u, 4u})
                for (unsigned op : {4u, 5u, 6u, 7u})
                    for (unsigned dst = 0; dst < 8; ++dst)
                        for (bool dead : {false, true}) {
                            std::vector<uint8_t> code = {0x04, 1};
                            if (width != 1 && (width == 4) != code32) code.push_back(0x66);
                            code.insert(code.end(), {uint8_t(width == 1 ? 0xd2 : 0xd3),
                                                    uint8_t(0xc0 | op << 3 | dst)});
                            if (dead) code.insert(code.end(), {0x85, 0xf6});
                            code.insert(code.end(), {0x73, 0});
                            for (auto c : {&reference, &actual}) {
                                c->s[CS].big = code32;
                                for (unsigned n = 0; n < code.size(); ++n) c->write(n, code[n], 1);
                            }
                            for (unsigned count = 0; count < 256; ++count)
                                for (uint32_t pattern : {0u, 0xffffffffu, 0x80008080u, 0x12345678u}) {
                                    for (auto c : {&reference, &actual}) {
                                        c->ip = c->instructions = 0;
                                        c->flags = 0x202 | ((count & 1) ? 0xcd5 : 0);
                                        for (unsigned n = 0; n < 8; ++n) c->r[n] = pattern ^ (n * 0x10101u);
                                        c->set(CX, count, 1);
                                    }
                                    for (unsigned k = 0; k < (dead ? 4u : 3u); ++k) reference.step();
                                    check(jit.run() == (dead ? 4u : 3u), "CL shifts with live/dead flags execute natively");
                                    equal(reference, actual);
                                }
                        }
        // Only AF remains live through the CL shift: a fixed shift overwrites
        // the other flags, and LAHF captures AF before XOR overwrites flags.
        for (bool code32 : {false, true})
            for (unsigned width : {1u, 2u, 4u}) {
                std::vector<uint8_t> code = {0x04, 1};
                if (width != 1 && (width == 4) != code32) code.push_back(0x66);
                code.insert(code.end(), {uint8_t(width == 1 ? 0xd2 : 0xd3), 0xe9,
                                        0xd1, 0xe2, 0x9f, 0x33, 0xdb, 0x73, 0});
                for (auto c : {&reference, &actual}) {
                    c->s[CS].big = code32;
                    for (unsigned n = 0; n < code.size(); ++n) c->write(n, code[n], 1);
                }
                for (unsigned count = 0; count < 256; ++count)
                    for (unsigned al : {15u, 16u, 127u, 255u}) {
                        for (auto c : {&reference, &actual}) {
                            c->ip = c->instructions = 0;
                            c->flags = 0xe96;
                            for (unsigned n = 0; n < 8; ++n) c->r[n] = 0x87654321u ^ n;
                            c->set(AX, al, 1); c->set(CX, count, 1);
                        }
                        for (unsigned n = 0; n < 6; ++n) reference.step();
                        check(jit.run() == 6, "AF-only shift dependency remains native");
                        equal(reference, actual);
                    }
            }
        // Ordinary flag-producing ALU/Jcc pairs must reach the normal warmed
        // native path too. Compare both branch outcomes with full prefix state.
        for (bool code32 : {false, true})
            for (unsigned width : {1u, 2u, 4u})
                for (unsigned op = 0; op < 8; ++op)
                    for (unsigned cc = 0; cc < 16; ++cc) {
                        CPU a(65536), b(65536);
                        Jit normal(b, true, 8, 65536);
                        unsigned dst = width == 1 ? 7 : BX, src = width == 1 ? 4 : DX;
                        std::vector<uint8_t> code;
                        if (width != 1 && (width == 4) != code32) code.push_back(0x66);
                        code.insert(code.end(), {uint8_t(op * 8 + (width != 1)),
                                                uint8_t(0xc0 | src << 3 | dst), uint8_t(0x70 | cc), 2});
                        for (auto c : {&a, &b}) {
                            c->s[CS].big = code32;
                            for (unsigned n = 0; n < code.size(); ++n) c->write(n, code[n], 1);
                        }
                        for (unsigned trial = 0; trial < 32; ++trial) {
                            for (auto c : {&a, &b}) {
                                c->ip = c->instructions = 0;
                                c->flags = 0xe96 | (trial & 1);
                                for (unsigned n = 0; n < 8; ++n) c->r[n] = 0xa5b6c7d8u ^ n;
                                c->set(dst, trial & 2 ? 0xffffffff : 0x7fff, width);
                                c->set(src, trial & 4 ? 1 : 0x80000000, width);
                            }
                            auto n = normal.run();
                            for (unsigned k = 0; k < n; ++k) a.step();
                            equal(a, b);
                            check(n == (trial < 7 ? 1u : 2u), "ALU/Jcc pair retains warmup then executes natively");
                        }
                    }
        auto carryCase = [&](unsigned width, unsigned dst, unsigned src,
                             uint32_t left, uint32_t right, unsigned carry) {
            for (auto c : {&reference, &actual}) {
                c->ip = c->instructions = 0;
                c->flags = 0xe96 | carry;
                for (unsigned n = 0; n < 8; ++n) c->r[n] = 0xa5b6c7d8u ^ (n * 0x10101u);
                c->set(dst, left, width); c->set(src, right, width);
            }
            reference.step(); reference.step(); reference.step();
            check(jit.run() == 3, "ADC/SBB and branch execute natively");
            equal(reference, actual);
        };
        const std::vector<uint32_t> carryEdges = {0, 1, 15, 16, 127, 128, 255,
            0x7fff, 0x8000, 0xffff, 0x7fffffff, 0x80000000, 0xffffffff};
        for (bool code32 : {false, true})
            for (unsigned width : {1u, 2u, 4u})
                for (unsigned op : {2u, 3u})
                    for (unsigned dst = 0; dst < 8; ++dst)
                        for (unsigned src = 0; src < 8; ++src) {
                            std::vector<uint8_t> code;
                            bool reverse = (dst ^ src) & 1;
                            if (width != 1 && (width == 4) != code32) code.push_back(0x66);
                            code.insert(code.end(), {uint8_t(op * 8 + (width != 1) + (reverse ? 2 : 0)),
                                uint8_t(0xc0 | (reverse ? dst << 3 | src : src << 3 | dst)), 0x90, 0x73, 0});
                            for (auto c : {&reference, &actual}) {
                                c->s[CS].big = code32;
                                for (unsigned n = 0; n < code.size(); ++n) c->write(n, code[n], 1);
                            }
                            if (width == 1 && dst == 0) {
                                for (unsigned a = 0; a < 256; ++a)
                                    for (unsigned b = 0; b < 256; ++b)
                                        for (unsigned cf = 0; cf < 2; ++cf) carryCase(width, dst, src, a, b, cf);
                            } else
                                for (auto a : carryEdges)
                                    for (auto b : carryEdges)
                                        for (unsigned cf = 0; cf < 2; ++cf) carryCase(width, dst, src, a, b, cf);
                        }
        // Immediate accumulator, full-width group, and sign-extended/alias group.
        for (bool code32 : {false, true})
            for (unsigned width : {1u, 2u, 4u})
                for (unsigned op : {2u, 3u})
                    for (unsigned form = 0; form < 3; ++form)
                        for (auto immediate : carryEdges) {
                            std::vector<uint8_t> code;
                            unsigned dst = form ? 7 : AX;
                            if (width != 1 && (width == 4) != code32) code.push_back(0x66);
                            if (!form) code.push_back(op * 8 + (width == 1 ? 4 : 5));
                            else code.insert(code.end(), {uint8_t(width == 1 ? (form == 1 ? 0x80 : 0x82)
                                                          : form == 1 ? 0x81 : 0x83),
                                                         uint8_t(0xc0 | op << 3 | dst)});
                            unsigned bytes = form == 2 ? 1 : width;
                            for (unsigned n = 0; n < bytes; ++n) code.push_back(immediate >> (n * 8));
                            code.insert(code.end(), {0x90, 0x73, 0});
                            for (auto c : {&reference, &actual}) {
                                c->s[CS].big = code32;
                                for (unsigned n = 0; n < code.size(); ++n) c->write(n, code[n], 1);
                            }
                            for (auto a : carryEdges)
                                for (unsigned cf = 0; cf < 2; ++cf) carryCase(width, dst, dst, a, a, cf);
                        }
        // Carry from a pending ADD and through multiple ADC/SBBs must survive
        // even when the final XOR makes the last arithmetic output flags dead.
        {
            std::mt19937 inputs(0xcadca11);
            const uint8_t code[] = {0x80,0xc2,1, 0x12,0xc4, 0x18,0xdf,
                                    0x80,0xd1,0xfe, 0x33,0xf6, 0x73,0};
            for (bool code32 : {false, true}) {
                for (auto c : {&reference, &actual}) {
                    c->s[CS].big = code32;
                    for (unsigned n = 0; n < sizeof(code); ++n) c->write(n, code[n], 1);
                }
                for (unsigned trial = 0; trial < 10000; ++trial) {
                    reference.ip = actual.ip = reference.instructions = actual.instructions = 0;
                    reference.flags = actual.flags = 0x202 | (inputs() & 0xcd5);
                    for (unsigned n = 0; n < 8; ++n) reference.r[n] = actual.r[n] = inputs();
                    for (unsigned n = 0; n < 6; ++n) reference.step();
                    check(jit.run() == 6, "pending and dead-output carry chains remain native");
                    equal(reference, actual);
                }
            }
        }
        // A trace side exit must expose the ADC's exact flags and prefix state;
        // the predicted continuation overwrites most flags with INC.
        for (bool code32 : {false, true}) {
            CPU a(65536), b(65536);
            Jit trace(b, true, 0, 65536, Jit::BlockMode::Trace);
            const uint8_t code[] = {0x04,1, 0x10,0xd3, 0x73,2, 0x46,0x90,0x40,0xf4};
            for (auto c : {&a, &b}) {
                c->s[CS].big = code32;
                for (unsigned n = 0; n < sizeof(code); ++n) c->write(n, code[n], 1);
            }
            for (unsigned al : {0u, 255u})
                for (auto left : carryEdges)
                    for (auto right : carryEdges) {
                        for (auto c : {&a, &b}) {
                            c->ip = c->instructions = 0;
                            c->flags = 0xad6;
                            for (unsigned n = 0; n < 8; ++n) c->r[n] = 0xa5b6c7d8u ^ n;
                            c->set(AX, al, 1); c->set(BX, left, 1); c->set(DX, right, 1);
                        }
                        auto n = trace.run();
                        check(n == 3 || n == 6, "carry trace executes a valid prefix or continuation");
                        for (unsigned k = 0; k < n; ++k) a.step();
                        equal(a, b);
                    }
        }
        // LAHF consumes pending flags even when the following XOR overwrites
        // them, and changes only AH. Exhaust byte ADD operands/aliases and
        // preserve externally supplied low flag bits in the flag-only form.
        for (bool code32 : {false, true})
            for (unsigned src = 0; src < 8; ++src) {
                const uint8_t code[] = {0x02, uint8_t(0xc0 | src), 0x9f, 0x33, 0xdb, 0x73, 0};
                for (auto c : {&reference, &actual}) {
                    c->s[CS].big = code32;
                    for (unsigned n = 0; n < sizeof(code); ++n) c->write(n, code[n], 1);
                }
                for (unsigned left = 0; left < 256; ++left)
                    for (unsigned right = 0; right < 256; ++right) {
                        for (auto c : {&reference, &actual}) {
                            c->ip = c->instructions = 0;
                            c->flags = 0x202 | ((right & 1) ? 0xcd5 : 0);
                            for (unsigned n = 0; n < 8; ++n) c->r[n] = 0xa5b6c7d8u ^ n;
                            c->set(AX, left, 1); c->set(src, right, 1);
                        }
                        reference.step();
                        auto expectedAH = reference.flags & 255;
                        reference.step(); reference.step(); reference.step();
                        check(jit.run() == 4, "ADD/LAHF/XOR/JNC execute natively");
                        equal(reference, actual);
                        check(actual.get(4, 1) == expectedAH, "LAHF captures flags before the later XOR");
                    }
            }
        for (bool code32 : {false, true}) {
            const uint8_t code[] = {0x9f, 0x90, 0x90, 0x73, 0};
            for (auto c : {&reference, &actual}) {
                c->s[CS].big = code32;
                for (unsigned n = 0; n < sizeof(code); ++n) c->write(n, code[n], 1);
            }
            for (unsigned low = 0; low < 256; ++low) {
                for (auto c : {&reference, &actual}) {
                    c->ip = c->instructions = 0;
                    c->flags = 0xa0000200u | low;
                    for (unsigned n = 0; n < 8; ++n) c->r[n] = 0x87654321u ^ n;
                }
                for (unsigned n = 0; n < 4; ++n) reference.step();
                check(jit.run() == 4, "LAHF alone observes supplied low flags natively");
                equal(reference, actual);
            }
        }
        // An unsupported rotate terminates this SHRD pair independently of CL
        // shift coverage. Test normal pair admission, not just warmup=0 lowering.
        for (bool code32 : {false, true})
            for (unsigned width : {2u, 4u}) {
                CPU a(65536), b(65536);
                Jit normal(b);
                std::vector<uint8_t> code;
                for (uint8_t modrm : {0xf8, 0xd7}) {
                    if ((width == 4) != code32) code.push_back(0x66);
                    code.insert(code.end(), {0x0f, 0xad, modrm});
                }
                const auto pairEnd = code.size();
                code.insert(code.end(), {0xd3, 0xca});
                for (auto c : {&a, &b}) {
                    c->s[CS].big = code32;
                    for (unsigned n = 0; n < code.size(); ++n) c->write(n, code[n], 1);
                }
                for (unsigned count = 0; count < 256; ++count)
                    for (unsigned visit = 0; visit < 8; ++visit) {
                        for (auto c : {&a, &b}) {
                            c->ip = c->instructions = 0;
                            c->flags = 0x202 | ((count & 1) ? 0xcd5 : 0);
                            for (unsigned n = 0; n < 8; ++n) c->r[n] = 0x87654321u ^ (n * 0x123458u);
                            c->set(CX, count, 1);
                        }
                        auto n = normal.run();
                        for (unsigned k = 0; k < n; ++k) a.step();
                        equal(a, b);
                        if (count || visit == 7) {
                            check(n == 2 && b.ip == pairEnd, "normal warmup compiles the SHRD pair only");
                            check(normal.nativeInstructions > 0, "SHRD pair reaches native execution");
                        } else
                            check(n == 1 && normal.nativeInstructions == 0, "SHRD pair retains normal warmup");
                    }
            }
        // Bit tests preserve all flags except CF, including pending ADD flags.
        // Exercise every immediate byte and all source/destination aliases;
        // register indices include negative and high-bit-set values.
        for (bool code32 : {false, true})
            for (unsigned width : {2u, 4u})
                for (bool immediate : {false, true})
                    for (unsigned kind = 4; kind < 8; ++kind)
                        for (unsigned dst = 0; dst < 8; ++dst)
                            for (unsigned src = 0; src < (immediate ? 1u : 8u); ++src)
                                for (unsigned index = 0; index < 256; ++index) {
                                    std::vector<uint8_t> code = {0x04, 1};
                                    if ((width == 4) != code32) code.push_back(0x66);
                                    const uint8_t operations[] = {0xa3, 0xab, 0xb3, 0xbb};
                                    code.insert(code.end(), {0x0f, immediate ? uint8_t(0xba) : operations[kind - 4],
                                        uint8_t(0xc0 | ((immediate ? kind : src) << 3) | dst)});
                                    if (immediate) code.push_back(index);
                                    code.insert(code.end(), {0x73, 0});
                                    for (auto c : {&reference, &actual}) {
                                        c->s[CS].big = code32;
                                        for (unsigned n = 0; n < code.size(); ++n) c->write(n, code[n], 1);
                                    }
                                    for (uint32_t pattern : {0u, 0xffffffffu, 0x80018001u}) {
                                        for (auto c : {&reference, &actual}) {
                                            c->ip = c->instructions = 0;
                                            c->flags = 0x202 | ((index & 1) ? 0xcd5 : 0);
                                            for (unsigned n = 0; n < 8; ++n) c->r[n] = pattern ^ (n * 0x10101u);
                                            if (!immediate) c->r[src] = (pattern & ~255u) | index;
                                        }
                                        reference.step(); reference.step(); reference.step();
                                        check(jit.run() == 3, "ADD/BT-family/JNC execute natively");
                                        equal(reference, actual);
                                    }
                                }
        // SHRD register forms: all register overlaps, full CL/immediate byte
        // range (including masked zero and 16-bit overshifts), and pending flags.
        for (bool code32 : {false, true})
            for (unsigned width : {2u, 4u})
                for (bool immediate : {false, true})
                    for (unsigned dst = 0; dst < 8; ++dst)
                        for (unsigned src = 0; src < 8; ++src)
                            for (unsigned count = 0; count < 256; ++count)
                                for (bool dead : {false, true}) {
                                    std::vector<uint8_t> code = {0x04, 1};
                                    if ((width == 4) != code32) code.push_back(0x66);
                                    code.insert(code.end(), {0x0f, uint8_t(immediate ? 0xac : 0xad),
                                                             uint8_t(0xc0 | (src << 3) | dst)});
                                    if (immediate) code.push_back(count);
                                    if (dead) code.insert(code.end(), {0x85, 0xf6});
                                    code.insert(code.end(), {0x73, 0});
                                    for (auto c : {&reference, &actual}) {
                                        c->s[CS].big = code32;
                                        for (unsigned n = 0; n < code.size(); ++n) c->write(n, code[n], 1);
                                    }
                                    for (uint32_t pattern : {0u, 0xffffffffu, 0x80008000u, 0x12345678u}) {
                                        for (auto c : {&reference, &actual}) {
                                            c->ip = c->instructions = 0;
                                            c->flags = 0x202 | ((count & 1) ? 0xcd5 : 0);
                                            for (unsigned n = 0; n < 8; ++n) c->r[n] = pattern ^ (n * 0x10101u);
                                            c->set(CX, count, 1);
                                        }
                                        for (unsigned k = 0; k < (dead ? 4u : 3u); ++k) reference.step();
                                        check(jit.run() == (dead ? 4u : 3u), "SHRD with live/dead flags executes natively");
                                        equal(reference, actual);
                                    }
                                }
        // A faulting load observes SHRD flags even if the following XOR would
        // overwrite them. Include runtime/immediate zeros and 16-bit overshifts.
        for (unsigned width : {2u, 4u})
            for (bool immediate : {false, true})
                for (unsigned count = 0; count < 256; ++count) {
                    CPU a(65536), b(65536);
                    std::vector<uint8_t> code = {0x04, 1};
                    if (width == 2) code.push_back(0x66);
                    code.insert(code.end(), {0x0f, uint8_t(immediate ? 0xac : 0xad), 0xd0});
                    if (immediate) code.push_back(count);
                    const auto faultIP = code.size();
                    code.insert(code.end(), {0x64,0x8b,0x1d,0,0,0,0, 0x33,0xf6});
                    for (auto c : {&a, &b}) {
                        c->s[CS].big = true;
                        c->cr[0] = 0x11;
                        c->s[FS].unusable = true;
                        std::copy(code.begin(), code.end(), c->mem.begin());
                        for (unsigned n = 0; n < 8; ++n) c->r[n] = 0x123456ffu ^ n;
                        c->set(CX, count, 1);
                    }
                    struct Stop {};
                    unsigned faults = 0;
                    auto observe = [&](CPU &c, const Fault &f) {
                        check(f.vector == 13 && c.ip == faultIP, "fault observes exact SHRD boundary");
                        ++faults;
                        throw Stop{};
                    };
                    a.observeFault = [&](const Fault &f) { observe(a, f); };
                    b.observeFault = [&](const Fault &f) { observe(b, f); };
                    try { for (unsigned n = 0; n < 3; ++n) a.step(); } catch (const Stop &) {}
                    Jit precise(b, true, 0, 65536);
                    try { precise.run(); } catch (const Stop &) {}
                    check(faults == 2, "both engines expose the fault before flag overwrite");
                    equal(a, b);
                }
        // Exhaust byte multiplication inputs, including AL/AH overlap and
        // arbitrary upper GPR halves. ADD produces pending flags before MUL;
        // a carry branch observes the resulting CF without a helper call.
        for (bool code32 : {false, true})
            for (unsigned source = 0; source < 8; ++source) {
                const uint8_t code[] = {0x04, 1, 0xf6, uint8_t(0xe0 | source), 0x73, 0};
                for (auto c : {&reference, &actual}) {
                    c->s[CS].big = code32;
                    for (unsigned n = 0; n < sizeof(code); ++n) c->write(n, code[n], 1);
                }
                for (unsigned left = 0; left < 256; ++left)
                    for (unsigned right = 0; right < 256; ++right) {
                        for (auto c : {&reference, &actual}) {
                            c->ip = c->instructions = 0;
                            c->flags = 0x202 | ((left & 1) ? 0xcd5 : 0);
                            for (unsigned n = 0; n < 8; ++n) c->r[n] = 0xa5b6c7d8u + n;
                            c->set(AX, left, 1);
                            c->set(source, right, 1);
                        }
                        reference.step();
                        auto multiplicand = reference.get(AX, 1);
                        auto multiplier = reference.get(source, 1);
                        reference.step();
                        reference.step();
                        check(jit.run() == 3, "ADD/MUL byte/JNC execute natively");
                        equal(reference, actual);
                        check(actual.get(AX, 2) == multiplicand * multiplier,
                              "MUL byte commits complete unsigned product to AX");
                    }
            }
        // Exhaust every byte value/alias and exercise signed overflow and
        // carry boundaries at both wider sizes, including operand overrides.
        for (bool code32 : {false, true})
            for (unsigned width : {1u, 2u, 4u})
                for (unsigned reg = 0; reg < 8; ++reg) {
                    std::vector<uint32_t> values;
                    if (width == 1)
                        for (unsigned value = 0; value < 256; ++value)
                            values.push_back(value);
                    else
                        values = {0, 1, 15, 16, 0x7f, 0x80, 0x7fff, 0x8000,
                                  0xffff, 0x7fffffff, 0x80000000, 0xffffffff};
                    for (auto value : values) {
                        std::vector<uint8_t> code;
                        if (width != 1 && (width == 4) != code32)
                            code.push_back(0x66);
                        code.insert(code.end(), {uint8_t(width == 1 ? 0xf6 : 0xf7),
                                                uint8_t(0xd8 | reg), 0x70, 0x10});
                        for (auto c : {&reference, &actual}) {
                            c->s[CS].big = code32;
                            c->ip = c->instructions = 0;
                            c->flags = 0x244ed7;
                            for (unsigned n = 0; n < 8; ++n)
                                c->r[n] = 0xa5b6c7d8u + n;
                            c->set(reg, value, width);
                            for (unsigned n = 0; n < code.size(); ++n)
                                c->write(n, code[n], 1);
                        }
                        reference.step();
                        reference.step();
                        check(jit.run() == 2, "NEG and overflow branch execute natively");
                        equal(reference, actual);
                        uint32_t operand = value & (width == 4 ? UINT32_MAX : (1u << (width * 8)) - 1);
                        check(bool(actual.flags & 1) == (operand != 0), "NEG carry iff nonzero operand");
                        check(bool(actual.flags & 0x800) == (operand == (1u << (width * 8 - 1))),
                              "NEG overflow iff minimum signed operand");
                    }
                }
        std::mt19937 rng(0xaadc130f);
        for (unsigned trial = 0; trial < 5000; ++trial) {
            bool code32 = rng() & 1;
            std::vector<uint8_t> code;
            unsigned length = 4 + rng() % 17;
            for (unsigned n = 0; n < length; ++n) {
                bool overrideWidth = rng() & 1;
                unsigned width = code32 != overrideWidth ? 4 : 2;
                unsigned dst = rng() & 7, src = rng() & 7;
                if (overrideWidth)
                    code.push_back(0x66);
                auto modrm = uint8_t(0xc0 | (src << 3) | dst);
                switch (rng() % 14) {
                case 0: code.insert(code.end(), {0x83, uint8_t(0xc0 | dst), uint8_t(rng())}); break;
                case 1: code.insert(code.end(), {0x31, modrm}); break;
                case 2: code.insert(code.end(), {0x83, uint8_t(0xf8 | dst), uint8_t(rng())}); break;
                case 3: code.insert(code.end(), {0x85, modrm}); break;
                case 4: code.push_back(0x40 | (rng() & 15)); break;
                case 5: code.insert(code.end(), {0x0f, 0xaf, modrm}); break;
                case 6:
                case 7: {
                    unsigned groups[] = {4, 5, 7};
                    code.insert(code.end(), {uint8_t(n & 1 ? 0xc0 : 0xc1),
                                uint8_t(0xc0 | (groups[rng() % 3] << 3) | dst), uint8_t(rng())});
                    break;
                }
                case 8: {
                    code.push_back(0xb8 | dst);
                    uint32_t value = rng();
                    for (unsigned k = 0; k < width; ++k)
                        code.push_back(value >> (k * 8));
                    break;
                }
                case 9: code.insert(code.end(), {0xfe, uint8_t(0xc0 | ((rng() & 1) << 3) | dst)}); break;
                case 10: code.insert(code.end(), {0x6b, modrm, uint8_t(rng())}); break;
                case 11: code.insert(code.end(), {0x87, modrm}); break;
                case 12: code.insert(code.end(), {0xf7, uint8_t(0xd8 | dst)}); break;
                case 13: code.insert(code.end(), {0xf6, uint8_t(0xd8 | dst)}); break;
                }
            }
            code.insert(code.end(), {uint8_t(0x70 | (trial & 15)), 0x10});
            for (auto c : {&reference, &actual}) {
                c->s[CS].big = code32;
                c->ip = c->instructions = 0;
                for (unsigned n = 0; n < code.size(); ++n)
                    c->write(n, code[n], 1);
            }
            reference.flags = actual.flags = 0x244602 | (rng() & 0x8d5);
            for (unsigned n = 0; n < 8; ++n)
                reference.r[n] = actual.r[n] = rng();
            for (unsigned n = 0; n <= length; ++n)
                reference.step();
            check(jit.run() == length + 1, "entire random instruction sequence executes natively");
            equal(reference, actual);
        }
        for (auto mode : {Jit::BlockMode::Basic, Jit::BlockMode::FollowJumps, Jit::BlockMode::Trace})
        for (unsigned pattern = 0; pattern < 5; ++pattern)
            for (unsigned boundary = 0; boundary < 2; ++boundary) {
                CPU expected(65536), native(65536);
                std::vector<uint8_t> code = pattern == 4
                    ? std::vector<uint8_t>{0x03, 0x05, 0, 0x40, 0, 0, 0x42, 0x43}
                    : pattern == 3
                    ? std::vector<uint8_t>{0xf7, 0xd8, 0x42, 0x43}
                    : pattern == 2
                    ? std::vector<uint8_t>{0x83, 0xf8, 1, 0x42, 0x43}
                    : std::vector<uint8_t>{0x83, 0xc0, 1, 0x0f, 0xaf, 0xd9, 0xc1,
                                          uint8_t(pattern ? 0xfa : 0xe2), uint8_t(pattern ? 2 : 1)};
                bool joining = mode != Jit::BlockMode::Basic;
                if (mode == Jit::BlockMode::Trace)
                    code.insert(code.end(), {0x74, 0}); // ZF is clear in all five patterns.
                else if (joining)
                    code.insert(code.begin(), {0xeb, 0});
                auto boundaryIP = code.size();
                if (!boundary)
                    code.insert(code.end(), {0x64, 0x8b, 0x35, 0, 0, 0, 0});
                else
                    code.insert(code.end(), {0xa3, 0, 1, 0, 0});
                auto continuation = code.size();
                code.insert(code.end(), {0x31, 0xff, 0x75, 0x10});
                struct Stop {};
                unsigned faults = 0;
                for (auto c : {&expected, &native}) {
                    c->cr[0] = 0x11;
                    c->s[CS].big = true;
                    c->s[FS].unusable = true;
                    c->r[AX] = 0x7fffffff;
                    c->r[BX] = 0x80000000;
                    c->r[CX] = 0xffffffff;
                    c->r[DX] = 0x80000001;
                    std::copy(code.begin(), code.end(), c->mem.begin());
                    c->mem[0x4000] = 1;
                    c->markCode(0x100); // Successful store must exit after code invalidation.
                    c->observeFault = [&, c](const Fault &f) {
                        ++faults;
                        check(f.vector == 13 && c->ip == boundaryIP,
                              "fault observes flags before the later overwrite");
                        throw Stop{};
                    };
                }
                try {
                    for (unsigned n = 0; n < 4 + unsigned(joining); ++n)
                        expected.step();
                } catch (const Stop &) {
                }
                Jit guarded(native, true, 0, 65536, mode);
                try {
                    guarded.run();
                } catch (const Stop &) {
                }
                equal(expected, native);
                if (!boundary)
                    check(faults == 2 && guarded.nativeInstructions == 3 + unsigned(joining),
                          "memory-fault boundary keeps earlier partial flag writers live");
                else {
                    check(native.ip == continuation && guarded.nativeInstructions == 4 + unsigned(joining) &&
                              native.mem == expected.mem,
                          "code-changing store exposes complete flags before later XOR");
                    expected.step();
                    expected.step();
                    check(guarded.run() == 2, "resume at the post-invalidation flag overwrite");
                    equal(expected, native);
                }
            }
        // Joined jumps must count executed branches, skip unreachable bytes,
        // retain pending flags, and stop at the same 64-instruction budget.
        for (unsigned trial = 0; trial < 1024; ++trial) {
            CPU expected(65536), native(65536);
            std::vector<uint8_t> code;
            unsigned length = 1 + rng() % 20;
            bool loop = trial & 1, code32 = trial & 2;
            for (unsigned n = 0; n < length; ++n) {
                code.insert(code.end(), {0x83, uint8_t(0xc0 | (rng() & 7)), uint8_t(rng()),
                                         0xeb, 1, 0xf4});
            }
            if (loop) {
                code.push_back(0xe9);
                unsigned width = code32 ? 4 : 2;
                int displacement = -int(code.size() + width);
                for (unsigned k = 0; k < width; ++k)
                    code.push_back(uint32_t(displacement) >> (k * 8));
            } else
                code.insert(code.end(), {uint8_t(0x70 | (trial & 15)), 0x10});
            for (auto c : {&expected, &native}) {
                c->s[CS].big = code32;
                c->flags = 0x202 | (rng() & 0x8d5);
                std::copy(code.begin(), code.end(), c->mem.begin());
            }
            expected.flags = native.flags;
            for (unsigned n = 0; n < 8; ++n)
                expected.r[n] = native.r[n] = rng();
            Jit block(native, true, 0, 65536, Jit::BlockMode::FollowJumps);
            unsigned count = block.run();
            check(count == (loop ? 64 : length * 2 + 1) && block.nativeInstructions == count,
                  "joined jump block counts every executed instruction");
            for (unsigned n = 0; n < count; ++n)
                expected.step();
            equal(expected, native);
        }
        for (unsigned target : {4096u, 100u}) {
            CPU expected(65536), native(65536);
            for (auto c : {&expected, &native}) {
                c->s[CS].big = true;
                c->mem[0] = 0xe9;
                c->write(1, target - 5, 4);
                c->mem[target] = 0xf4;
            }
            Jit block(native, true, 0, 65536, Jit::BlockMode::FollowJumps);
            check(block.run() == 1 && block.nativeInstructions == 1,
                  "jump to another page or unsupported target remains terminal");
            expected.step();
            equal(expected, native);
        }
        // Every Jcc condition and arithmetic-flag image exercises both trace
        // predictions, including the precise prefix count on an unexpected exit.
        for (unsigned condition = 0; condition < 16; ++condition)
            for (unsigned pattern = 0; pattern < 64; ++pattern)
                for (bool backward : {false, true})
                    for (bool code32 : {false, true})
                    for (bool terminal : {false, true}) {
                        CPU expected(65536), native(65536);
                        std::vector<uint8_t> code = backward
                            ? std::vector<uint8_t>{0x40, 0xeb, 2, 0xf4, 0xf4,
                                                  uint8_t(0x70 | condition), 0xf9, 0xf4}
                            : std::vector<uint8_t>{uint8_t(0x70 | condition), 3,
                                                  0x31, 0xdb, 0x90, 0xf4};
                        if (terminal)
                            code = backward
                                ? std::vector<uint8_t>{0xf4, 0x90, 0x90, 0x90, 0x90,
                                                      uint8_t(0x70 | condition), 0xf9, 0xf4}
                                : std::vector<uint8_t>{uint8_t(0x70 | condition), 3,
                                                      0xf4, 0x90, 0x90, 0x40, 0xf4};
                        unsigned flags = 0x202;
                        unsigned bits[] = {CF, PF, AF, ZF, SF, OF};
                        for (unsigned k = 0; k < 6; ++k)
                            if (pattern & (1u << k))
                                flags |= bits[k];
                        for (auto c : {&expected, &native}) {
                            c->s[CS].big = code32;
                            c->ip = backward ? 5 : 0;
                            c->flags = flags;
                            std::copy(code.begin(), code.end(), c->mem.begin());
                        }
                        for (unsigned k = 0; k < 8; ++k)
                            expected.r[k] = native.r[k] = rng();
                        Jit block(native, true, 0, 65536, Jit::BlockMode::Trace);
                        unsigned count = block.run();
                        check(count >= 1 && count <= (terminal ? 1u : 64u) && block.nativeInstructions == count,
                              "conditional trace executes an exact bounded native prefix");
                        for (unsigned k = 0; k < count; ++k)
                            expected.step();
                        equal(expected, native);
                    }
        // A comparison before an internal branch remains observable on its
        // side exit even though XOR overwrites every arithmetic flag afterward.
        for (unsigned trial = 0; trial < 4096; ++trial) {
            CPU expected(65536), native(65536);
            std::vector<uint8_t> code = {0x83, 0xf8, uint8_t(trial >> 4),
                uint8_t(0x70 | (trial & 15)), 3, 0x31, 0xdb, 0x90, 0xf4};
            for (auto c : {&expected, &native}) {
                c->s[CS].big = trial & 16;
                c->flags = 0x202;
                std::copy(code.begin(), code.end(), c->mem.begin());
            }
            for (unsigned k = 0; k < 8; ++k)
                expected.r[k] = native.r[k] = rng();
            if (trial & 32)
                expected.r[AX] = native.r[AX] = uint32_t(int32_t(int8_t(trial >> 4)));
            Jit block(native, true, 0, 65536, Jit::BlockMode::Trace);
            unsigned count = block.run();
            check((count == 2 || count == 4) && block.nativeInstructions == count,
                  "CMP side exit precedes the predicted path's flag overwrite");
            for (unsigned k = 0; k < count; ++k)
                expected.step();
            equal(expected, native);
        }
        // Long blocks exercise late flag producers and the maximum native
        // instruction budget with mixed partial/full flag dependencies.
        for (unsigned length : {49u, 50u, 51u, 63u, 64u}) {
            CPU expected(65536), native(65536);
            std::vector<uint8_t> code;
            for (unsigned n = 0; n < length; ++n)
                switch (n & 3) {
                case 0: code.insert(code.end(), {0x83, 0xc0, 1}); break;
                case 1: code.insert(code.end(), {0x0f, 0xaf, 0xd9}); break;
                case 2: code.insert(code.end(), {0xc1, 0xfa, 2}); break;
                case 3: code.push_back(0x46); break;
                }
            code.push_back(0xf4); // End decoding without extending this block.
            for (auto c : {&expected, &native}) {
                c->s[CS].big = true;
                c->r[AX] = 0x7fffffff;
                c->r[BX] = 0x80000000;
                c->r[CX] = 0xffffffff;
                c->r[DX] = 0x80000001;
                std::copy(code.begin(), code.end(), c->mem.begin());
            }
            for (unsigned n = 0; n < length; ++n)
                expected.step();
            Jit block(native, true, 0, 65536);
            check(block.run() == length, "long block reconstructs flags from high recipe slots");
            equal(expected, native);
        }
        std::cout << "Passed " << checks << " flag-liveness checks\n";
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << " after " << checks << " checks\n";
        return 1;
    }
}
