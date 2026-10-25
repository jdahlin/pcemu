#include "cpu.hpp"
#include "jit.hpp"
#include <algorithm>
#include <cstring>
#include <iostream>
using namespace pc;
static unsigned checks;
static std::string context;
static void check(bool ok, const char *message) {
    ++checks;
    if (!ok) throw std::runtime_error(std::string(message) + " " + context);
}
static void equal(const CPU &a, const CPU &b) {
    if (a.ip != b.ip || a.flags != b.flags || a.instructions != b.instructions || a.cr[2] != b.cr[2])
        std::cerr << "expected ip/flags/count/cr2 " << std::hex << a.ip << '/' << a.flags << '/' << a.instructions << '/' << a.cr[2]
                  << " actual " << b.ip << '/' << b.flags << '/' << b.instructions << '/' << b.cr[2] << std::dec << '\n';
    check(a.ip == b.ip && a.flags == b.flags && a.instructions == b.instructions && a.cr[2] == b.cr[2],
          "RMW flags/IP/count/CR2 mismatch");
    for (unsigned reg = 0; reg < 8; ++reg)
        check(a.r[reg] == b.r[reg], "RMW changed guest GPR");
    check(a.mem == b.mem, "RMW memory side effects mismatch");
    check(a.generation == b.generation, "RMW code generation side effects mismatch");
}
static std::vector<uint8_t> code(bool big, bool address32, unsigned width, bool decrement,
                                 bool fs, uint32_t address) {
    std::vector<uint8_t> bytes{0x04, 0xff}; // Pending producer supplies INC/DEC's preserved CF.
    if (width != 1 && (width == 4) != big) bytes.push_back(0x66);
    if (address32 != big) bytes.push_back(0x67);
    if (fs) bytes.push_back(0x64);
    bytes.push_back(width == 1 ? 0xfe : 0xff);
    bytes.push_back((decrement ? 8 : 0) | (address32 ? 5 : 6));
    for (unsigned n = 0; n < (address32 ? 4u : 2u); ++n) bytes.push_back(address >> (8 * n));
    return bytes;
}
int main() {
    try {
        CPU reference(65536), actual(65536);
        Jit jit(actual, true, 0, 65536);
        if (!jit.enabled()) { std::cout << "RMW tests require Apple ARM64\n"; return 0; }
        const std::array<uint32_t, 14> values = {0, 1, 2, 15, 16, 0x7f, 0x80, 0xff, 0x7fff,
                                   0x8000, 0xffff, 0x7fffffff, 0x80000000, 0xffffffff};
        for (bool big : {false, true})
        for (bool address32 : {false, true})
        for (unsigned width : {1u, 2u, 4u})
        for (bool decrement : {false, true})
        for (unsigned carry = 0; carry < 2; ++carry)
        for (unsigned condition = 0; condition < 16; ++condition)
        for (unsigned sample = 0; sample < (width == 1 ? 256u : values.size()); ++sample) {
            auto value = width == 1 ? sample : values[sample];
            context = "flat width=" + std::to_string(width) + " value=" + std::to_string(value);
            bool fs = condition & 1;
            auto bytes = code(big, address32, width, decrement, fs, 0x4000);
            bytes.insert(bytes.end(), {uint8_t(0x70 | condition), 3});
            for (auto c : {&reference, &actual}) {
                c->s[CS].big = big; c->s[FS].base = 0x1000;
                c->ip = c->instructions = 0; c->flags = 0x244e97;
                for (unsigned reg = 0; reg < 8; ++reg) c->r[reg] = 0x12345600 + reg;
                c->r[AX] |= carry;
                for (unsigned n = 0; n < bytes.size(); ++n) c->write(n, bytes[n], 1);
                c->write(0x4000 + (fs ? 0x1000 : 0), value, width);
            }
            // Rewriting the next case's code invalidates JIT-marked bytes;
            // compare execution deltas from the same generation baseline.
            reference.generation = actual.generation;
            for (unsigned n = 0; n < 3; ++n) reference.step();
            auto native = jit.nativeInstructions, helpers = actual.nativeMemoryCalls;
            check(jit.run() == 3 && jit.nativeInstructions - native == 3,
                  "INC/DEC and branch must execute natively");
            check(actual.nativeMemoryCalls == helpers, "flat RMW should use inline RAM");
            equal(reference, actual);
        }
        for (unsigned access = 0; access < 12; ++access)
        for (unsigned width : {1u, 2u, 4u})
        for (bool decrement : {false, true})
        for (unsigned carry = 0; carry < 2; ++carry) {
            context = "replay access=" + std::to_string(access) + " width=" + std::to_string(width);
            CPU ref(2 * 1024 * 1024), native(2 * 1024 * 1024);
            uint32_t address = access == 11 ? 0x109000 : access == 5 ? 0xa0000 : access == 10 ? 0x100 :
                               (access == 7 || access == 8) ? 0x9fff : 0x9000;
            bool paging = access != 4 && access != 10 && access != 11;
            auto bytes = code(true, true, width, decrement, access == 4, address);
            auto branchIP = bytes.size();
            bytes.insert(bytes.end(), {0x72, 3});
            unsigned refReads = 0, nativeReads = 0, refWrites = 0, nativeWrites = 0, faults = 0;
            std::vector<uint32_t> refTrace, nativeTrace;
            for (auto c : {&ref, &native}) {
                c->s[CS].big = true; c->r[AX] = carry; c->flags = 0x202;
                std::copy(bytes.begin(), bytes.end(), c->mem.begin());
                c->write(access == 11 ? 0x9000 : address, 0x7fffffff, width);
                c->write(0x80000, 0x81007, 4);
                c->write(0x81000, 7, 4);
                c->write(0x81024, 0x9007 | (access == 1 ? 64 : 0), 4);
                c->write(0x81028, access == 8 ? 0xa005 : 0xa007, 4);
                c->write(0x81000 + 0xa0 * 4, 0xa0007, 4);
                if (access == 2 || access == 9) c->write(0x81024, 0x9005, 4);
                if (access == 3) c->write(0x81024, 0, 4);
                c->cr[0] = paging ? 0x80000011 : 0x11; c->cr[3] = 0x80000;
                c->s[CS].selector = 3; c->cr[2] = 0x12345678; c->a20 = access != 11;
                c->s[FS].unusable = access == 4;
                if (access == 1) c->physical(address, true);
                if (access == 10) c->markCode(address);
                c->videoRead = [&, c](uint32_t) {
                    (c == &ref ? refReads : nativeReads)++;
                    (c == &ref ? refTrace : nativeTrace).push_back(c->mem[0x81000 + 0xa0 * 4] & 64);
                    return uint8_t(0xff);
                };
                c->videoWrite = [&, c](uint32_t, uint8_t value) {
                    (c == &ref ? refWrites : nativeWrites)++;
                    (c == &ref ? refTrace : nativeTrace).push_back(0x100 | value | ((c->flags & CF) << 16));
                };
                if (access == 6 || access == 9) {
                    c->watchStart = address; c->watchLength = width;
                    c->watchWrite = [&, c](uint32_t, uint32_t value, int) {
                        (c == &ref ? refTrace : nativeTrace).push_back(value);
                        (c == &ref ? refTrace : nativeTrace).push_back(c->flags);
                    };
                }
            }
            struct Stop {};
            ref.observeFault = native.observeFault = [&](const Fault &f) {
                ++faults;
                check(f.vector == (access == 4 ? 13 : 14), "RMW precise fault vector");
                check(f.hasError && f.error == (access == 4 ? 0u : access == 3 ? 4u : 7u),
                      "RMW fault preserves read/write permission error code");
                throw Stop{};
            };
            try { while (ref.instructions < 3) ref.step(); } catch (const Stop &) {}
            Jit guarded(native, true, 0);
            try { while (native.instructions < 3) guarded.run(); } catch (const Stop &) {}
            bool fault = access == 2 || access == 3 || access == 4 || access == 9 || (access == 8 && width > 1);
            check(faults == (fault ? 2u : 0u), "RMW replay must fault once per engine");
            equal(ref, native);
            check(refTrace == nativeTrace && refReads == nativeReads && refWrites == nativeWrites,
                  "RMW replay duplicated callbacks or changed flags/dirty ordering");
            if (fault) check(native.ip == 2u, "RMW faults at original prefixed instruction");
            if (access == 1 || access == 11)
                check(native.nativeMemoryCalls == 0, "warm writable/dirty or A20-masked RMW must inline");
            if (access == 10) check(guarded.nativeInstructions == 3 && native.ip >= branchIP,
                                    "tracked code RMW resumes after generation change");
        }
        // A later fault/code write must publish the RMW flags before a
        // decoded successor overwrites them, including preserved carry.
        for (unsigned width : {1u, 2u, 4u})
        for (bool decrement : {false, true})
        for (unsigned carry = 0; carry < 2; ++carry)
        for (bool codeWrite : {false, true}) {
            context = "later boundary width=" + std::to_string(width);
            CPU ref(65536), native(65536);
            auto bytes = code(true, true, width, decrement, false, 0x9000);
            auto boundaryIP = bytes.size();
            if (codeWrite) bytes.insert(bytes.end(), {0xa2, 0, 1, 0, 0});
            else bytes.insert(bytes.end(), {0x64, 0x8b, 0x15, 0, 0, 0, 0});
            auto continuation = bytes.size();
            bytes.insert(bytes.end(), {0x31, 0xc0, 0x75, 3});
            for (auto c : {&ref, &native}) {
                c->s[CS].big = true; c->cr[0] = 0x11; c->s[FS].unusable = true;
                c->r[AX] = carry; c->flags = 0x202;
                std::copy(bytes.begin(), bytes.end(), c->mem.begin());
                c->write(0x9000, (1u << (width * 8 - 1)) - 1, width);
                c->markCode(0x100);
            }
            struct Stop {};
            unsigned faults = 0;
            ref.observeFault = native.observeFault = [&](const Fault &f) {
                ++faults;
                check(f.vector == 13, "following access should fault precisely");
                throw Stop{};
            };
            try { for (unsigned n = 0; n < 3; ++n) ref.step(); } catch (const Stop &) {}
            Jit guarded(native, true, 0);
            try { guarded.run(); } catch (const Stop &) {}
            equal(ref, native);
            check(faults == (codeWrite ? 0u : 2u), "RMW later boundary fault count");
            check(native.ip == (codeWrite ? continuation : boundaryIP), "RMW later boundary IP");
            if (codeWrite) {
                ref.step(); ref.step();
                check(guarded.run() == 2, "resume after code-changing successor");
                equal(ref, native);
            }
        }
        std::cout << "Passed " << checks << " RMW JIT checks\n";
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << " after " << checks << " checks\n"; return 1;
    }
}
