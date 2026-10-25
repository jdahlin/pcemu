#include "jit.hpp"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <random>
using namespace pc;
static unsigned checks = 0;
static void check(bool value, const char *message) {
    ++checks;
    if (!value)
        throw std::runtime_error(message);
}
int main() {
    try {
        // Target validation precedes stack writes, including stack faults and
        // MMIO. Exercise direct and indirect near calls in all execution modes.
        for (unsigned mode = 0; mode < 3; ++mode)
            for (bool word : {false, true})
                for (unsigned form = 0; form < 3; ++form)
                    for (bool native : {false, true}) {
                        CPU c(1024 * 1024);
                        c.s[CS].big = true;
                        c.s[CS].limit = 0x100;
                        c.cr[0] = mode ? 0x11 : 0x10;
                        c.flags = 0x202 | (mode == 2 ? 0x20000 : 0);
                        c.s[SS].big = true;
                        c.r[SP] = 0xa0004;
                        c.s[SS].unusable = mode == 1;
                        std::vector<uint8_t> code = {0x04, 1};
                        c.r[AX] = 0x123456ff;
                        unsigned width = word ? 2 : 4;
                        if (word) code.push_back(0x66);
                        if (!form) {
                            code.push_back(0xe8);
                            auto displacement = 0x101u - (code.size() + width);
                            for (unsigned n = 0; n < width; ++n) code.push_back(displacement >> (n * 8));
                        } else if (form == 1) {
                            code.insert(code.end(), {0xff, 0xd3});
                            c.r[BX] = 0x101;
                        } else {
                            code.insert(code.end(), {0xff, 0x15, 0, 0x40, 0, 0});
                            c.write(0x4000, 0x101, width);
                        }
                        std::copy(code.begin(), code.end(), c.mem.begin());
                        unsigned writes = 0;
                        c.videoWrite = [&](uint32_t, uint8_t) { ++writes; };
                        struct Stop {};
                        bool fault = false;
                        c.observeFault = [&](const Fault &f) {
                            check(f.vector == 13 && f.error == 0 && c.ip == 2 &&
                                      c.r[AX] == 0x12345600 && (c.flags & 0x8d5) == 0x55,
                                  "near CALL target limit faults at original IP");
                            fault = true;
                            throw Stop{};
                        };
                        Jit guarded(c, native, 0, 65536);
                        try { for (unsigned n = 0; n < 3; ++n) guarded.run(); } catch (const Stop &) {}
                        check(fault && writes == 0 && c.r[SP] == 0xa0004,
                              "invalid target precedes stack fault and MMIO without changing SP");
                    }
        {
            CPU c(65536);
            c.cr[0] = 0x11;
            c.s[CS].big = c.s[SS].big = true;
            c.r[SP] = 0x9000;
            c.s[CS].limit = 0x101;
            const uint8_t code[] = {0xe8, 0xfc, 0, 0, 0};
            std::copy(std::begin(code), std::end(code), c.mem.begin());
            Jit cached(c, true, 0, 65536);
            check(cached.run() == 1 && c.ip == 0x101 && c.r[SP] == 0x8ffc,
                  "near CALL initially succeeds within cached CS limit");
            c.ip = 0; c.r[SP] = 0x9000; c.s[CS].limit = 0x100;
            c.write(0x8ffc, 0xdeadbeef, 4);
            struct Stop {};
            bool fault = false;
            c.observeFault = [&](const Fault &f) {
                fault = f.vector == 13 && c.ip == 0;
                throw Stop{};
            };
            try { cached.run(); } catch (const Stop &) {}
            check(fault && c.r[SP] == 0x9000 && c.read(0x8ffc, 4) == 0xdeadbeef,
                  "cached near CALL rechecks a changed CS limit before pushing");
        }
        CPU expected(1024 * 1024), actual(1024 * 1024);
        Jit jit(actual, true, 0, 65536);
        std::mt19937 rng(58031);
        for (bool code32 : {false, true})
            for (bool operandOverride : {false, true})
                for (bool addressOverride : {false, true})
                    for (bool store : {false, true})
                        for (unsigned reg = 0; reg < 10; ++reg)
                            for (bool paging : {false, true})
                                for (unsigned edge : {0u, 1u, 2u, 3u})
                                    // Innermost: change SS.B without changing code or CS.
                                    for (bool stack32 : {false, true}) {
                                        if (reg >= 8 && !store)
                                            continue;
                                        unsigned width = code32 != operandOverride ? 4 : 2;
                                        std::vector<uint8_t> code = {0x83, 0xc3, 1, 0x64};
                                        if (operandOverride)
                                            code.push_back(0x66);
                                        if (addressOverride)
                                            code.push_back(0x67);
                                        if (reg == 8) {
                                            code.push_back(0x68);
                                            uint32_t value = rng();
                                            for (unsigned n = 0; n < width; ++n)
                                                code.push_back(value >> (n * 8));
                                        } else if (reg == 9) {
                                            code.push_back(0x6a);
                                            code.push_back(edge & 1 ? 0x80 : 0x7f);
                                        } else
                                            code.push_back((store ? 0x50 : 0x58) | reg);
                                        code.push_back(0x70 | (edge * 4 + reg % 4));
                                        code.push_back(0);
                                        uint32_t offset = 0x7000 + edge;
                                        for (auto c : {&expected, &actual}) {
                                            c->cr[0] = 1;
                                            c->flushTLB();
                                            c->ip = 0;
                                            c->instructions = 0;
                                            c->halted = false;
                                            c->s[CS].big = code32;
                                            c->s[SS].big = stack32;
                                            c->s[SS].base = 0x10000;
                                            c->s[FS].unusable = true;
                                            std::copy(code.begin(), code.end(), c->mem.begin());
                                            c->written(0, code.size());
                                            for (unsigned n = 0; n < 16; ++n)
                                                c->mem[0x10000 + offset - 8 + n] =
                                                    uint8_t(n * 31 + edge);
                                            if (paging) {
                                                c->write(0x80000, 0x81007, 4);
                                                for (unsigned page = 0; page < 256; ++page)
                                                    c->write(0x81000 + page * 4, (page << 12) | 7,
                                                             4);
                                                c->cr[3] = 0x80000;
                                                c->cr[0] = 0x80000001;
                                            }
                                        }
                                        for (unsigned r = 0; r < 8; ++r)
                                            expected.r[r] = actual.r[r] = rng();
                                        expected.r[SP] = actual.r[SP] =
                                            offset | (stack32 ? 0 : 0xabcd0000);
                                        expected.flags = actual.flags = 2 | (rng() & 0x8d5);
                                        for (unsigned n = 0; n < 3; ++n)
                                            expected.step();
                                        unsigned completed = 0;
                                        while (actual.instructions < 3)
                                            completed += jit.run();
                                        check(completed == 3 && actual.ip == expected.ip &&
                                                  actual.flags == expected.flags,
                                              "native stack preserves flags, IP and instruction "
                                              "count");
                                        for (unsigned r = 0; r < 8; ++r)
                                            check(actual.r[r] == expected.r[r],
                                                  "native stack preserves mixed-width SP and POP "
                                                  "destination ordering");
                                        auto location = 0x10000 + offset - (store ? width : 0);
                                        check(std::memcmp(actual.mem.data() + location,
                                                          expected.mem.data() + location,
                                                          width) == 0,
                                              "native stack reads/writes exact stack bytes");
                                    }
        check(jit.nativeInstructions > 5000 && jit.cacheRecycles > 0,
              "stack tests exercise native blocks and arena recycling");
        {
            CPU c(1024 * 1024);
            c.s[CS].big = true;
            const uint8_t code[] = {0xb8, 0x78, 0x56, 0x34, 0x12, 0x50, 0x5b, 0x70, 0};
            std::copy(std::begin(code), std::end(code), c.mem.begin());
            Jit cached(c, true, 0);
            for (bool stack32 : {false, true, false}) {
                c.s[SS].big = stack32;
                c.r[SP] = 0x17000;
                c.r[BX] = 0;
                c.ip = 0;
                auto location = stack32 ? 0x16ffc : 0x6ffc;
                std::fill_n(c.mem.begin() + location, 4, 0);
                check(cached.run() == 4 && c.r[SP] == 0x17000 && c.r[BX] == 0x12345678 &&
                          c.read(location, 4) == 0x12345678,
                      "cached stack blocks revalidate SS.B without code or CS changes");
            }
        }
        for (unsigned target : {unsigned(AX), unsigned(SP)}) {
            CPU c(65536);
            c.s[CS].big = c.s[SS].big = true;
            const uint8_t code[] = {0xff, uint8_t(0xd0 | target)};
            std::copy(std::begin(code), std::end(code), c.mem.begin());
            c.r[AX] = 0x100; c.r[SP] = 5;
            Jit changed(c, true, 0, 65536);
            check(changed.run() == 1 && changed.nativeInstructions == 1 &&
                      c.ip == (target == SP ? 5u : 0x100u) && c.r[SP] == 1 && c.read(1, 4) == 2,
                  "self-modifying register CALL commits captured target and return address once");
        }
        {
            CPU c(1024 * 1024);
            c.s[CS].big = c.s[SS].big = true;
            c.mem[0] = 0xff; c.mem[1] = 0xd0;
            c.r[AX] = 0x100; c.r[SP] = 0xa0004;
            unsigned accesses = 0;
            c.videoWrite = [&](uint32_t at, uint8_t value) {
                ++accesses;
                check(c.r[SP] == 0xa0000 && value == (at == 0xa0000 ? 2 : 0),
                      "register CALL MMIO sees committed SP and exact return bytes");
            };
            Jit guarded(c, true, 0, 65536);
            check(guarded.run() == 1 && guarded.nativeInstructions == 0 && accesses == 4 &&
                      c.ip == 0x100 && c.r[SP] == 0xa0000,
                  "register CALL MMIO replays exactly once");
        }
        for (bool store : {false, true})
            for (bool word : {false, true})
                for (bool stack32 : {false, true})
                    for (unsigned kind = 0; kind < 4; ++kind)
                      for (unsigned transfer = 0; transfer < 4; ++transfer) {
                        bool call = transfer == 1 || transfer == 3, ret = transfer == 2;
                        if ((call && !store) || (ret && store))
                            continue;
                        if (kind == 1 && !store)
                            continue;
                        CPU ref(1024 * 1024), native(1024 * 1024);
                        std::vector<uint8_t> code = {0xb8, 0xff, 0xff, 0xff, 0xff, 0x83, 0xc0, 1};
                        if (word)
                            code.push_back(0x66);
                        if (transfer == 3) code.insert(code.end(), {0xff, 0xd0});
                        else code.push_back(ret ? 0xc2 : call ? 0xe8 : store ? 0x50 : 0x5c);
                        if (ret)
                            code.insert(code.end(), {0xff, 0xff});
                        if (transfer == 1)
                            code.insert(code.end(), word ? 2 : 4, 0);
                        code.insert(code.end(), {0xbb, 0xef, 0xbe, 0xad, 0xde, 0xf4});
                        auto originalSP =
                            (0x9000u + (store ? (word ? 2 : 4) : 0)) | (stack32 ? 0 : 0xabcd0000u);
                        for (auto c : {&ref, &native}) {
                            std::copy(code.begin(), code.end(), c->mem.begin());
                            c->s[CS].big = true;
                            c->s[CS].selector = 3;
                            c->s[SS].big = stack32;
                            c->s[SS].unusable = kind == 3;
                            c->r[SP] = originalSP;
                            c->write(0x80000, 0x81007, 4);
                            c->write(0x81000, 7, 4);
                            if (kind == 1 || kind == 2)
                                c->write(0x81024, kind == 1 ? 0x9005 : 0x9003, 4);
                            c->cr[3] = 0x80000;
                            c->cr[0] = 0x80000001;
                        }
                        struct Stop {};
                        unsigned faults = 0;
                        auto observe = [&](CPU &c, const Fault &f) {
                            ++faults;
                            auto error = kind == 0 ? (store ? 6u : 4u) : (store ? 7u : 5u);
                            check(f.vector == (kind == 3 ? 12 : 14) &&
                                      f.error == (kind == 3 ? 0 : error) && c.ip == 8 &&
                                      c.r[SP] == originalSP && c.r[AX] == 0 && c.r[BX] == 0 &&
                                      (c.flags & (CF | ZF | PF | AF)) == (CF | ZF | PF | AF),
                                  "stack faults preserve SP, prior effects, flags and restart IP");
                            check(kind == 3 || c.cr[2] == 0x9000,
                                  "stack faults report the exact linear stack address");
                            throw Stop{};
                        };
                        ref.observeFault = [&](const Fault &f) { observe(ref, f); };
                        native.observeFault = [&](const Fault &f) { observe(native, f); };
                        try {
                            ref.step();
                            ref.step();
                            ref.step();
                        } catch (const Stop &) {
                        }
                        Jit guarded(native, true, 0);
                        try {
                            guarded.run();
                        } catch (const Stop &) {
                        }
                        check(faults == 2 && native.instructions == 3 &&
                                  guarded.nativeInstructions == 2 && native.mem == ref.mem &&
                                  native.flags == ref.flags,
                              "native stack fault replays once with an exact interpreter state");
                    }
        for (bool store : {false, true}) {
            CPU c(1024 * 1024);
            c.s[CS].big = c.s[SS].big = true;
            const uint8_t code[] = {0xb8, 0x44, 0x33, 0x22, 0x11, uint8_t(store ? 0x50 : 0x5c),
                                    0xf4};
            std::copy(std::begin(code), std::end(code), c.mem.begin());
            c.r[SP] = store ? 0xa0004 : 0xa0000;
            unsigned accesses = 0;
            c.videoRead = [&](uint32_t) {
                ++accesses;
                return uint8_t(0x12);
            };
            c.videoWrite = [&](uint32_t at, uint8_t value) {
                ++accesses;
                check(c.r[SP] == 0xa0000 && value == uint8_t(0x11223344u >> ((at & 3) * 8)),
                      "MMIO PUSH observes decremented SP and exact source bytes");
            };
            Jit guarded(c, true, 0);
            check(guarded.run() == 2 && guarded.nativeInstructions == 1 && accesses == 4 &&
                      c.r[SP] == (store ? 0xa0000u : 0x12121212u),
                  "MMIO stack fallback executes once with POP ESP ordering");
        }
        {
            CPU c(65536);
            c.s[CS].big = c.s[SS].big = true;
            const uint8_t code[] = {0xb8, 0x44, 0x33, 0x22, 0x11, 0x50, 0xbb, 0, 0, 0, 0, 0xf4};
            std::copy(std::begin(code), std::end(code), c.mem.begin());
            c.r[SP] = 11;
            Jit changed(c, true, 0);
            check(changed.run() == 2 && c.ip == 6 && c.r[SP] == 7 && c.r[BX] == 0,
                  "self-modifying PUSH commits SP then exits before stale native code");
            check(changed.run() == 1 && c.r[BX] == 0x11223344,
                  "self-modifying PUSH updates the following instruction");
        }
        for (bool code32 : {false, true})
            for (bool overrideWidth : {false, true})
                for (bool stack32 : {false, true})
                    for (bool paging : {false, true})
                        for (unsigned edge = 0; edge < 4; ++edge)
                            for (unsigned form = 0; form < 9; ++form) {
                                CPU ref(1024 * 1024), native(1024 * 1024);
                                unsigned width = code32 != overrideWidth ? 4 : 2;
                                std::vector<uint8_t> code = {0x83, 0xc3, 1};
                                if (overrideWidth)
                                    code.push_back(0x66);
                                if (!form) code.push_back(0xe8);
                                else code.insert(code.end(), {0xff, uint8_t(0xd0 | (form - 1))});
                                uint32_t displacement = edge & 1 ? 0xffffffe0u : 0x12345u;
                                if (!form) for (unsigned n = 0; n < width; ++n)
                                    code.push_back(displacement >> (n * 8));
                                for (auto c : {&ref, &native}) {
                                    c->cr[0] = 1;
                                    c->s[CS].big = code32;
                                    c->s[CS].limit = UINT32_MAX; // This fixture tests valid wide targets/wraps.
                                    c->s[SS].big = stack32;
                                    c->s[SS].base = 0x10000;
                                    c->r[SP] = (0x7000 + edge) | (stack32 ? 0 : 0xabcd0000u);
                                    c->r[BX] = 0x7fffffff;
                                    if (form && form - 1 != SP) c->r[form - 1] = 0x12345678u + edge;
                                    c->flags = 0x8d7;
                                    std::copy(code.begin(), code.end(), c->mem.begin());
                                    if (paging) {
                                        c->write(0x80000, 0x81007, 4);
                                        for (unsigned page = 0; page < 256; ++page)
                                            c->write(0x81000 + page * 4, (page << 12) | 7, 4);
                                        c->cr[3] = 0x80000;
                                        c->cr[0] = 0x80000001;
                                    }
                                    c->flushTLB();
                                }
                                ref.step();
                                ref.step();
                                Jit translated(native, true, 0);
                                check(translated.run() == 2 && translated.nativeInstructions == (edge && edge < width ? 1u : 2u),
                                      "near CALL runs natively or replays a page-crossing push");
                                check(native.ip == ref.ip && native.flags == ref.flags,
                                      "near CALL preserves target and preceding arithmetic flags");
                                for (unsigned r = 0; r < 8; ++r)
                                    check(native.r[r] == ref.r[r], "near CALL preserves registers");
                                check(std::memcmp(native.mem.data() + 0x16000,
                                                  ref.mem.data() + 0x16000, 0x2000) == 0,
                                      "near CALL pushes the exact mixed-width return address");
                            }
        {
            CPU c(65536);
            c.s[CS].big = c.s[SS].big = true;
            const uint8_t code[] = {0xe8, 5, 0, 0, 0};
            std::copy(std::begin(code), std::end(code), c.mem.begin());
            c.r[SP] = 5;
            Jit changed(c, true, 0);
            check(changed.run() == 1 && changed.nativeInstructions == 1 && c.ip == 10 &&
                      c.r[SP] == 1 && c.read(1, 4) == 5,
                  "self-modifying CALL commits its target after writing the return address");
        }
        {
            CPU c(1024 * 1024);
            c.s[CS].big = c.s[SS].big = true;
            const uint8_t code[] = {0xe8, 5, 0, 0, 0};
            std::copy(std::begin(code), std::end(code), c.mem.begin());
            c.r[SP] = 0xa0004;
            unsigned accesses = 0;
            c.videoWrite = [&](uint32_t at, uint8_t value) {
                ++accesses;
                check(c.r[SP] == 0xa0000 && value == (at == 0xa0000 ? 5 : 0),
                      "CALL MMIO replay observes the committed SP and return bytes");
            };
            Jit guarded(c, true, 0);
            check(guarded.run() == 1 && guarded.nativeInstructions == 0 && accesses == 4 &&
                      c.r[SP] == 0xa0000 && c.ip == 10,
                  "CALL MMIO replay executes the stack write and branch exactly once");
        }
        for (bool code32 : {false, true})
            for (bool overrideWidth : {false, true})
                for (bool stack32 : {false, true})
                    for (bool paging : {false, true})
                        for (unsigned extra : {0u, 1u, 4095u, 4096u, 65535u})
                            for (unsigned edge = 0; edge < 4; ++edge) {
                                CPU ref(1024 * 1024), native(1024 * 1024);
                                unsigned width = code32 != overrideWidth ? 4 : 2;
                                std::vector<uint8_t> code = {0x83, 0xc3, 1};
                                if (overrideWidth)
                                    code.push_back(0x66);
                                code.push_back(extra ? 0xc2 : 0xc3);
                                if (extra) {
                                    code.push_back(extra);
                                    code.push_back(extra >> 8);
                                }
                                for (auto c : {&ref, &native}) {
                                    c->cr[0] = 1;
                                    c->s[CS].big = code32;
                                    c->s[SS].big = stack32;
                                    c->s[SS].base = 0x10000;
                                    c->r[SP] = (0x7ffc + edge) | (stack32 ? 0 : 0xabcd0000u);
                                    c->r[BX] = 0x7fffffff;
                                    c->r[AX] = 0x12345678;
                                    c->flags = 0x8d7;
                                    std::copy(code.begin(), code.end(), c->mem.begin());
                                    c->write(0x17ffc + edge, 0x87654321, width);
                                    if (paging) {
                                        c->write(0x80000, 0x81007, 4);
                                        for (unsigned page = 0; page < 256; ++page)
                                            c->write(0x81000 + page * 4, (page << 12) | 7, 4);
                                        c->cr[3] = 0x80000;
                                        c->cr[0] = 0x80000001;
                                    }
                                    c->flushTLB();
                                }
                                ref.step();
                                ref.step();
                                Jit translated(native, true, 0);
                                check(translated.run() == 2 && translated.nativeInstructions ==
                                          (edge + width > 4 ? 1u : 2u),
                                      "RET runs natively or replays a page-crossing read");
                                check(native.ip == ref.ip && native.flags == ref.flags,
                                      "RET commits its dynamic target and preceding flags");
                                for (unsigned r = 0; r < 8; ++r)
                                    check(native.r[r] == ref.r[r],
                                          "RET preserves GPRs and wraps its stack adjustment");
                                check(native.mem == ref.mem, "RET preserves memory and paging bits");
                            }
        {
            CPU c(1024 * 1024);
            c.s[CS].big = c.s[SS].big = true;
            const uint8_t code[] = {0xc2, 0xff, 0xff};
            std::copy(std::begin(code), std::end(code), c.mem.begin());
            c.r[SP] = 0xa0000;
            unsigned accesses = 0;
            c.videoRead = [&](uint32_t at) {
                ++accesses;
                check(c.r[SP] == 0xa0000, "RET MMIO read sees the original stack pointer");
                return uint8_t(0x12345678u >> ((at & 3) * 8));
            };
            Jit guarded(c, true, 0);
            check(guarded.run() == 1 && guarded.nativeInstructions == 0 && accesses == 4 &&
                      c.r[SP] == 0xb0003 && c.ip == 0x12345678,
                  "RET MMIO replay reads and adjusts the stack exactly once");
        }
        std::cout << "Passed " << checks << " stack JIT checks\n";
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << " after " << checks << " checks\n";
        return 1;
    }
}
