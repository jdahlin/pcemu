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
static std::vector<uint8_t> instruction(unsigned kind, unsigned width, unsigned reg, bool big,
                                        bool address32, unsigned segment, uint32_t address,
                                        uint32_t value) {
    std::vector<uint8_t> code;
    if (width != 1 && (width == 4) != big)
        code.push_back(0x66);
    if (address32 != big)
        code.push_back(0x67);
    if (segment)
        code.push_back(segment == 1 ? 0x64 : 0x65);
    auto byte = [&](uint8_t v) { code.push_back(v); };
    auto imm = [&](uint32_t v, unsigned n) {
        for (unsigned i = 0; i < n; ++i)
            byte(v >> (i * 8));
    };
    unsigned group = reg;
    if (kind <= 1)
        byte((kind == 0 ? 0x38 : 0x3a) + (width != 1));
    else if (kind == 2) {
        byte(width == 1 ? 0x80 : 0x81);
        group = 7;
    } else if (kind == 3) {
        byte(width == 1 ? 0x82 : 0x83);
        group = 7;
    } else if (kind == 4)
        byte(width == 1 ? 0x84 : 0x85);
    else {
        byte(width == 1 ? 0xf6 : 0xf7);
        group = 0;
    }
    byte((group << 3) | (address32 ? 5 : 6));
    imm(address, address32 ? 4 : 2);
    if (kind == 2 || kind == 5)
        imm(value, width);
    if (kind == 3)
        imm(value, 1);
    return code;
}
int main() {
    try {
        CPU reference(65536), actual(65536);
        Jit jit(actual, true, 0, 65536);
        if (!jit.enabled()) {
            std::cout << "Read JIT tests require Apple ARM64\n";
            return 0;
        }
        std::mt19937 rng(0x839038);
        for (bool big : {false, true})
            for (bool address32 : {false, true})
                for (unsigned width : {1, 2, 4})
                    for (unsigned kind = 0; kind < 6; ++kind)
                        for (unsigned reg = 0; reg < 8; ++reg)
                            for (unsigned cc = 0; cc < 16; ++cc) {
                                unsigned segment = cc % 3;
                                uint32_t data = rng(), value = rng();
                                if (cc < 8) {
                                    const uint32_t edge[] = {0,    1,      0x7f,       0x80,
                                                             0xff, 0x7fff, 0x80000000, 0xffffffff};
                                    data = edge[cc];
                                    value = edge[7 - cc];
                                }
                                auto compare = instruction(kind, width, reg, big, address32,
                                                           segment, 0x4000, value);
                                std::vector<uint8_t> code = {
                                    0x04, 0x7f}; // prior flags must survive replay.
                                code.insert(code.end(), compare.begin(), compare.end());
                                code.push_back(0x70 | cc);
                                code.push_back(1);
                                code.push_back(0x90);
                                for (unsigned r = 0; r < 8; ++r)
                                    reference.r[r] = actual.r[r] = rng();
                                reference.flags = actual.flags = 2 | (rng() & 0x8d5);
                                for (auto c : {&reference, &actual}) {
                                    c->s[CS].big = big;
                                    c->ip = 0;
                                    c->instructions = 0;
                                    c->halted = false;
                                    c->s[FS].base = 0x1000;
                                    c->s[GS].base = 0x2000;
                                    for (unsigned i = 0; i < code.size(); ++i)
                                        c->write(i, code[i], 1);
                                    c->write(0x4000 + segment * 0x1000, data, width);
                                }
                                reference.step();
                                reference.step();
                                reference.step();
                                auto before = jit.nativeInstructions;
                                auto n = jit.run();
                                check(n == 3 && jit.nativeInstructions - before == 3,
                                      "memory CMP/TEST actually execute in native blocks");
                                check(actual.flags == reference.flags &&
                                          actual.ip == reference.ip && actual.instructions == 3,
                                      "memory comparison exact flags and Jcc");
                                for (unsigned r = 0; r < 8; ++r)
                                    check(actual.r[r] == reference.r[r],
                                          "comparison never changes guest registers");
                                check(actual.mem == reference.mem,
                                      "comparison never writes guest RAM");
                            }
        // Faults, MMIO, and valid crossing accesses must replay once with the
        // comparison's original IP and the flags of the preceding arithmetic.
        for (unsigned kind = 0; kind < 6; ++kind)
            for (unsigned width : {1, 2, 4})
                for (unsigned access = 0; access < 6; ++access) {
                    CPU ref(1024 * 1024), native(1024 * 1024);
                    unsigned address = access == 3 ? 0xa0000 : access >= 4 ? 0x9fff : 0x9000;
                    auto compare = instruction(kind, width, 4, true, true, 0, address, 0x80);
                    std::vector<uint8_t> code = {0xb8, 0xff, 0xff, 0xbb, 0xaa, 0x80, 0xc4, 1};
                    code.insert(code.end(), compare.begin(), compare.end());
                    code.push_back(0x74);
                    code.push_back(0);
                    unsigned refReads = 0, nativeReads = 0;
                    for (auto c : {&ref, &native}) {
                        std::copy(code.begin(), code.end(), c->mem.begin());
                        c->s[CS].big = true;
                        c->write(0x80000, 0x81007, 4);
                        c->write(0x81000, 7, 4);
                        if (access == 1)
                            c->write(0x81024, 0x9003, 4); // supervisor only
                        if (access >= 3) {
                            c->write(0x81024, 0x9007, 4);
                            c->write(0x81028, 0xa007, 4);
                            c->write(0x81000 + 0xa0 * 4, 0xa0007, 4);
                        }
                        c->cr[3] = 0x80000;
                        c->cr[0] = access == 2 ? 0x11 : 0x80000011;
                        c->s[CS].selector = 3;
                        c->s[DS].unusable = access == 2;
                        c->cr[2] = 0x12345;
                        if (access == 1) {
                            c->s[CS].selector = 0;
                            c->physical(0x9000);
                            c->s[CS].selector = 3;
                        }
                        if (access == 5) {
                            c->physical(address);
                            c->physical(address + 3);
                        }
                        c->videoRead = [&, c](uint32_t) {
                            (c == &ref ? refReads : nativeReads)++;
                            return uint8_t(0x80);
                        };
                    }
                    struct Stop {};
                    unsigned faults = 0;
                    auto observe = [&](CPU &c, const Fault &f) {
                        check(f.vector == (access == 2 ? 13 : 14), "comparison fault vector");
                        check(c.ip == 8 && c.r[AX] == 0xaabb00ff &&
                                  (c.flags & (CF | ZF | PF | AF | SF | OF)) == (CF | ZF | PF | AF),
                              "fault preserves prior arithmetic and original IP");
                        ++faults;
                        throw Stop{};
                    };
                    ref.observeFault = [&](const Fault &f) { observe(ref, f); };
                    native.observeFault = [&](const Fault &f) { observe(native, f); };
                    try {
                        while (ref.instructions < 4)
                            ref.step();
                    } catch (const Stop &) {
                    }
                    Jit guarded(native, true, 0);
                    try {
                        while (native.instructions < 4)
                            guarded.run();
                    } catch (const Stop &) {
                    }
                    check(faults == (access < 3 ? 2u : 0u), "fault observed once per engine");
                    check(native.ip == ref.ip && native.flags == ref.flags &&
                              native.instructions == ref.instructions && native.cr[2] == ref.cr[2],
                          "comparison replay matches interpreter state");
                    for (unsigned r = 0; r < 8; ++r)
                        check(native.r[r] == ref.r[r], "comparison replay register equality");
                    check(native.mem == ref.mem && refReads == nativeReads,
                          "no duplicate device read or page side effects");
                }
        // A later fault must publish comparison flags, not an earlier ALU's flags.
        {
            CPU c(65536);
            c.s[CS].big = true;
            c.s[DS].base = 0;
            const uint8_t code[] = {0xb8, 0, 0,    0,    0,    0x3b, 0x05, 0, 0x40,
                                    0,    0, 0x64, 0x8b, 0x05, 0,    0,    0, 0};
            std::copy(std::begin(code), std::end(code), c.mem.begin());
            c.mem[0x4000] = 1;
            c.cr[0] = 0x11;
            c.s[FS].unusable = true;
            struct Stop {};
            bool fault = false;
            c.observeFault = [&](const Fault &f) {
                fault = f.vector == 13 && c.ip == 11 && (c.flags & CF) && !(c.flags & ZF);
                throw Stop{};
            };
            Jit j(c, true, 0);
            try {
                j.run();
            } catch (const Stop &) {
            }
            check(fault, "later memory fault retains CMP operands and flags");
        }
        // Address arithmetic uses cached, recently modified registers, including
        // AH as a comparison operand sharing the address base's EAX register.
        for (bool big : {false, true}) {
            CPU ref(65536), native(65536);
            std::vector<uint8_t> code =
                big ? std::vector<uint8_t>{0xb8, 0, 0x40, 0,    0,    0xb9, 3,    0,
                                           0,    0, 0x3a, 0x64, 0x88, 1,    0x75, 0}
                    : std::vector<uint8_t>{0xbd, 0,    0x30, 0xbf, 3, 0,    0xb8,
                                           0,    0x40, 0x3a, 0x63, 1, 0x75, 0};
            for (auto c : {&ref, &native}) {
                c->s[CS].big = big;
                c->s[SS].base = 0x1000;
                std::copy(code.begin(), code.end(), c->mem.begin());
                c->mem[big ? 0x400d : 0x4004] = 0x40;
            }
            unsigned count = big ? 4 : 5;
            for (unsigned k = 0; k < count; ++k)
                ref.step();
            Jit j(native, true, 0);
            check(j.run() == count, "cached SIB/BP-index comparison lowered");
            check(native.ip == ref.ip && native.flags == ref.flags,
                  "cached addressing preserves comparison and branch");
            for (unsigned r = 0; r < 8; ++r)
                check(native.r[r] == ref.r[r], "address operand aliases preserve registers");
        }
        // A following self-modifying store must exit with the comparison's
        // flags and must not execute an already-decoded stale immediate.
        {
            CPU c(65536);
            c.s[CS].big = true;
            const uint8_t code[] = {0xb8, 0,    0,    0,    0,    0x3b, 0x05, 0, 0x40,
                                    0,    0,    0xb2, 0x7f, 0x88, 0x15, 21,   0, 0,
                                    0,    0xbb, 0x11, 0x11, 0x11, 0x11, 0xf4};
            std::copy(std::begin(code), std::end(code), c.mem.begin());
            c.mem[0x4000] = 1;
            Jit j(c, true, 0);
            check(j.run() == 4 && c.ip == 19 && c.r[BX] == 0 && (c.flags & CF),
                  "SMC exit commits comparison flags");
            check(j.run() == 1 && c.r[BX] == 0x11117f11,
                  "SMC restarts from modified comparison successor");
        }
        // Immediate TEST forms share flag finalization with register TEST but
        // must preserve AL/AH and every unaffected bit of the destination GPR.
        for (bool big : {false, true})
            for (unsigned width : {1u, 2u, 4u})
                for (unsigned reg = 0; reg < 8; ++reg)
                    for (unsigned cc = 0; cc < 16; ++cc) {
                        CPU ref(65536), native(65536);
                        std::vector<uint8_t> code;
                        if (width != 1 && (width == 4) != big)
                            code.push_back(0x66);
                        bool accumulator = reg == 0 && (cc & 1);
                        if (accumulator)
                            code.push_back(width == 1 ? 0xa8 : 0xa9);
                        else {
                            code.push_back(width == 1 ? 0xf6 : 0xf7);
                            code.push_back(0xc0 | reg);
                        }
                        uint32_t value = rng();
                        for (unsigned b = 0; b < width; ++b)
                            code.push_back(value >> (8 * b));
                        code.push_back(0x70 | cc);
                        code.push_back(1);
                        code.push_back(0x90);
                        for (unsigned r = 0; r < 8; ++r)
                            ref.r[r] = native.r[r] = rng();
                        ref.flags = native.flags = 2 | (rng() & 0x8d5);
                        for (auto c : {&ref, &native}) {
                            c->s[CS].big = big;
                            std::copy(code.begin(), code.end(), c->mem.begin());
                        }
                        ref.step();
                        ref.step();
                        Jit j(native, true, 0);
                        check(j.run() == 2 && j.nativeInstructions == 2,
                              "immediate register TEST is native");
                        check(native.ip == ref.ip && native.flags == ref.flags,
                              "immediate register TEST flags and all conditions");
                        for (unsigned r = 0; r < 8; ++r)
                            check(native.r[r] == ref.r[r],
                                  "immediate TEST preserves every GPR bit");
                    }
        for (unsigned form = 0; form < 3; ++form) {
            CPU c(65536);
            c.s[CS].big = true;
            c.r[AX] = 1;
            std::vector<uint8_t> code = form == 0   ? std::vector<uint8_t>{0x3d, 0, 0, 0, 0}
                                        : form == 1 ? std::vector<uint8_t>{0xa8, 1}
                                                    : std::vector<uint8_t>{0xf6, 0xc4, 1};
            if (form == 2)
                c.r[AX] = 0x100;
            code.push_back(0x75);
            code.push_back(uint8_t(-int(code.size() + 1)));
            std::copy(code.begin(), code.end(), c.mem.begin());
            Jit j(c, true);
            for (unsigned n = 0; n < 40; ++n)
                j.run();
            check(j.nativeInstructions > 0 && c.ip == 0,
                  "register compare/test branch pairs pass production warmup");
        }
        // Production warmup must admit a CMP/TEST + conditional-branch pair,
        // rather than recording a permanent negative entry for the hot loop.
        for (unsigned kind : {2u, 5u})
            for (unsigned width : {1u, 2u, 4u}) {
                CPU c(65536);
                c.s[CS].big = true;
                auto code = instruction(kind, width, 0, true, true, 0, 0x4000, kind == 2 ? 0 : 1);
                code.push_back(0x75);
                code.push_back(uint8_t(-int(code.size() + 1)));
                std::copy(code.begin(), code.end(), c.mem.begin());
                c.mem[0x4000] = 1;
                Jit hot(c, true);
                for (unsigned n = 0; n < 40; ++n)
                    hot.run();
                check(hot.nativeInstructions > 0 && hot.cacheHits > 0 && c.ip == 0,
                      "production warmup compiles hot memory comparison/branch pairs");
            }
        check(jit.cacheRecycles > 0, "memory comparison arena recycling exercised");
        std::cout << "Passed " << checks << " read JIT checks\n";
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << " after " << checks << " checks\n";
        return 1;
    }
}
