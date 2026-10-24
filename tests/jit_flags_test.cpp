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
                switch (rng() % 12) {
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
        for (bool joining : {false, true})
        for (unsigned pattern = 0; pattern < 3; ++pattern)
            for (unsigned boundary = 0; boundary < 2; ++boundary) {
                CPU expected(65536), native(65536);
                std::vector<uint8_t> code = pattern == 2
                    ? std::vector<uint8_t>{0x83, 0xf8, 1, 0x42, 0x43}
                    : std::vector<uint8_t>{0x83, 0xc0, 1, 0x0f, 0xaf, 0xd9, 0xc1,
                                          uint8_t(pattern ? 0xfa : 0xe2), uint8_t(pattern ? 2 : 1)};
                if (joining)
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
                Jit guarded(native, true, 0, 65536, joining ? Jit::BlockMode::FollowJumps
                                                         : Jit::BlockMode::Basic);
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
