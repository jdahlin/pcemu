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
        for (bool store : {false, true})
            for (bool word : {false, true})
                for (bool stack32 : {false, true})
                    for (unsigned kind = 0; kind < 4; ++kind)
                      for (bool call : {false, true}) {
                        if (call && !store)
                            continue;
                        if (kind == 1 && !store)
                            continue;
                        CPU ref(1024 * 1024), native(1024 * 1024);
                        std::vector<uint8_t> code = {0xb8, 0xff, 0xff, 0xff, 0xff, 0x83, 0xc0, 1};
                        if (word)
                            code.push_back(0x66);
                        code.push_back(call ? 0xe8 : store ? 0x50 : 0x5c);
                        if (call)
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
                        for (unsigned edge = 0; edge < 4; ++edge) {
                            CPU ref(1024 * 1024), native(1024 * 1024);
                            unsigned width = code32 != overrideWidth ? 4 : 2;
                            std::vector<uint8_t> code = {0x83, 0xc3, 1};
                            if (overrideWidth)
                                code.push_back(0x66);
                            code.push_back(0xe8);
                            uint32_t displacement = edge & 1 ? 0xffffffe0u : 0x12345u;
                            for (unsigned n = 0; n < width; ++n)
                                code.push_back(displacement >> (n * 8));
                            for (auto c : {&ref, &native}) {
                                c->cr[0] = 1;
                                c->s[CS].big = code32;
                                c->s[SS].big = stack32;
                                c->s[SS].base = 0x10000;
                                c->r[SP] = (0x7000 + edge) | (stack32 ? 0 : 0xabcd0000u);
                                c->r[BX] = 0x7fffffff;
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
                                  "direct CALL runs natively or replays a page-crossing push");
                            check(native.ip == ref.ip && native.flags == ref.flags,
                                  "direct CALL preserves target and preceding arithmetic flags");
                            for (unsigned r = 0; r < 8; ++r)
                                check(native.r[r] == ref.r[r], "direct CALL preserves registers");
                            check(std::memcmp(native.mem.data() + 0x16000,
                                              ref.mem.data() + 0x16000, 0x2000) == 0,
                                  "direct CALL pushes the exact mixed-width return address");
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
        std::cout << "Passed " << checks << " stack JIT checks\n";
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << " after " << checks << " checks\n";
        return 1;
    }
}
