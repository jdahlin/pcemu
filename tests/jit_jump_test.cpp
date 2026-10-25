#include "cpu.hpp"
#include "jit.hpp"
#include <algorithm>
#include <iostream>
using namespace pc;
static unsigned checks;
static void check(bool ok, const char *message) {
    ++checks;
    if (!ok) throw std::runtime_error(message);
}
static void equal(const CPU &a, const CPU &b) {
    check(a.ip == b.ip, "indirect jump target mismatch");
    check(a.flags == b.flags, "indirect jump changed flags");
    check(a.instructions == b.instructions, "indirect jump instruction count mismatch");
    check(std::equal(std::begin(a.r), std::end(a.r), std::begin(b.r)), "indirect jump changed GPRs/stack");
    check(a.mem == b.mem, "indirect jump memory side effects mismatch");
    check(a.cr[2] == b.cr[2], "indirect jump CR2 mismatch");
}
static std::vector<uint8_t> instruction(bool big, unsigned width, bool address32,
                                        int reg, bool pending, unsigned form = 0) {
    std::vector<uint8_t> bytes;
    if (pending) bytes = {0x04, 0xff}; // ADD AL,-1 supplies pending arithmetic flags.
    if ((width == 4) != big) bytes.push_back(0x66);
    if (address32 != big) bytes.push_back(0x67);
    if (reg >= 0) bytes.insert(bytes.end(), {0xff, uint8_t(0xe0 | reg)});
    else {
        if (form == 0) { // FS override, absolute displacement.
            bytes.insert(bytes.end(), {0x64, 0xff, uint8_t(address32 ? 0x25 : 0x26)});
            for (unsigned n = 0; n < (address32 ? 4u : 2u); ++n)
                bytes.push_back(0x4000 >> (n * 8));
        } else if (address32) // SS:[EBP + ESI*4 + 0x10]
            bytes.insert(bytes.end(), {0xff, 0x64, 0xb5, 0x10});
        else // SS:[BP+DI+0x10]
            bytes.insert(bytes.end(), {0xff, 0x63, 0x10});
    }
    return bytes;
}
int main() {
    try {
        CPU probe(65536); Jit available(probe);
        if (!available.enabled()) { std::cout << "Indirect jump tests require ARM64\n"; return 0; }
        for (auto mode : {Jit::BlockMode::Basic, Jit::BlockMode::FollowJumps, Jit::BlockMode::Trace})
        for (bool big : {false, true})
        for (unsigned width : {2u, 4u})
        for (bool address32 : {false, true})
        for (bool pending : {false, true})
        for (int reg = -2; reg < 8; ++reg)
        for (auto target : {0u, 0x1234u, 0x12345678u, 0xffffffffu}) {
            CPU ref(65536), cpu(65536);
            auto bytes = instruction(big, width, address32, reg < 0 ? -1 : reg, pending, reg == -2);
            for (auto c : {&ref, &cpu}) {
                c->s[CS].big = big; c->flags = 0x244e97;
                c->s[FS].base = c->s[SS].base = 0x1000;
                for (unsigned r = 0; r < 8; ++r) c->r[r] = 0x56780000 + r;
                c->r[BP] = address32 ? 0x3fe0 : 0xaaaa3fe0;
                c->r[SI] = 4; c->r[DI] = 0xbbbb0010;
                if (reg >= 0) c->r[reg] = target;
                c->write(0x5000, target, width);
                std::copy(bytes.begin(), bytes.end(), c->mem.begin());
            }
            Jit jit(cpu, true, 0, 65536, mode);
            for (unsigned n = 0; n < (pending ? 2u : 1u); ++n) ref.step();
            check(jit.run() == (pending ? 2u : 1u), "indirect jump block length");
            check(jit.nativeInstructions == (pending ? 2u : 1u), "indirect jump must be native");
            equal(ref, cpu);
            // Same compiled instruction must read the new pointer, not bake its target.
            if (reg < 0) {
                ref.ip = cpu.ip = 0;
                ref.write(0x5000, 0x76543210, width); cpu.write(0x5000, 0x76543210, width);
                for (unsigned n = 0; n < (pending ? 2u : 1u); ++n) ref.step();
                jit.run(); equal(ref, cpu);
            }
        }
        // A single indirect transfer is admitted after normal warmup too.
        {
            CPU cpu(65536); cpu.s[CS].big = true;
            cpu.mem[0] = 0xff; cpu.mem[1] = 0xe3; cpu.r[BX] = 0;
            Jit jit(cpu);
            for (unsigned n = 0; n < 32; ++n) jit.run();
            check(jit.nativeInstructions > 0, "single jump rejected after warmup");
        }
        // Read-only pages are valid targets. Faults and devices must replay once,
        // with preceding flags committed and the original prefixed instruction IP.
        for (unsigned access = 0; access < 7; ++access)
        for (unsigned width : {2u, 4u}) {
            CPU ref(2 * 1024 * 1024), cpu(2 * 1024 * 1024);
            auto bytes = instruction(true, width, true, -1, true);
            unsigned refReads = 0, cpuReads = 0, faults = 0;
            for (auto c : {&ref, &cpu}) {
                c->s[CS].big = true; c->s[CS].selector = 3; c->r[AX] = 1;
                c->s[FS].base = access == 4 ? 0x9c000 : access == 5 ? 0x5fff : 0x5000;
                c->s[FS].unusable = access == 3;
                c->write(0x9000, 0x12345678, width);
                c->write(0x80000, 0x81007, 4); c->write(0x81000, 7, 4);
                c->write(0x81024, access == 1 ? 0 : access == 2 ? 0x9003 : 0x9005, 4);
                c->write(0x81028, 0xa007, 4);
                c->write(0x81000 + 0xa0 * 4, 0xa0007, 4);
                c->cr[0] = 0x80000011; c->cr[3] = 0x80000;
                if (access == 6) c->physical(0x9000);
                c->videoRead = [&, c](uint32_t) { ++(c == &ref ? refReads : cpuReads); return uint8_t(0x12); };
                std::copy(bytes.begin(), bytes.end(), c->mem.begin());
            }
            struct Stop {};
            ref.observeFault = cpu.observeFault = [&](const Fault &f) {
                ++faults;
                check(f.vector == (access == 3 ? 13 : 14), "jump read fault vector");
                check(f.error == (access == 3 ? 0u : access == 1 ? 4u : 5u), "jump read fault error");
                throw Stop{};
            };
            try { ref.step(); ref.step(); } catch (const Stop &) {}
            Jit jit(cpu, true, 0);
            try { while (cpu.instructions < 2) jit.run(); } catch (const Stop &) {}
            equal(ref, cpu);
            check(faults == (access >= 1 && access <= 3 ? 2u : 0u), "jump replay fault count");
            check(refReads == cpuReads, "jump replay duplicated MMIO read");
            if (access == 4) check(cpuReads == width, "jump device callbacks missing");
            if (access == 6) check(cpu.nativeMemoryCalls == 0, "warm read-only pointer must inline");
        }
        // The destination fetch is a separate instruction boundary: a jump may
        // commit an unmapped target, then fault there on the following dispatch.
        {
            CPU ref(2 * 1024 * 1024), cpu(2 * 1024 * 1024);
            for (auto c : {&ref, &cpu}) {
                c->s[CS].big = true; c->s[CS].selector = 3;
                c->mem[0] = 0xff; c->mem[1] = 0xe3; c->r[BX] = 0x9000;
                c->write(0x80000, 0x81007, 4); c->write(0x81000, 7, 4);
                c->cr[0] = 0x80000011; c->cr[3] = 0x80000;
            }
            Jit jit(cpu, true, 0); ref.step(); jit.run(); equal(ref, cpu);
            check(cpu.ip == 0x9000 && jit.nativeInstructions == 1, "jump must commit before target fetch");
            unsigned faults = 0;
            struct Stop {};
            ref.observeFault = cpu.observeFault = [&](const Fault &f) {
                check(f.vector == 14 && f.error == 4, "target fetch fault");
                ++faults; throw Stop{};
            };
            try { ref.step(); } catch (const Stop &) {}
            try { jit.run(); } catch (const Stop &) {}
            equal(ref, cpu); check(faults == 2, "target fetch must fault once per engine");
        }
        // A20 aliasing must affect pointer reads but not truncate the jump target.
        {
            CPU ref(2 * 1024 * 1024), cpu(2 * 1024 * 1024);
            auto bytes = instruction(true, 4, true, -1, true);
            for (auto c : {&ref, &cpu}) {
                c->s[CS].big = true; c->s[FS].base = 0x100000;
                c->a20 = false; c->write(0x4000, 0xabcdef01, 4);
                std::copy(bytes.begin(), bytes.end(), c->mem.begin());
            }
            Jit jit(cpu, true, 0); ref.step(); ref.step(); jit.run(); equal(ref, cpu);
            check(cpu.ip == 0xabcdef01 && jit.nativeInstructions == 2, "A20 pointer jump target");
        }
        // A preceding tracked-code write must exit before executing a decoded
        // jump, then reload the freshly changed pointer at the next dispatch.
        {
            CPU ref(65536), cpu(65536);
            const std::vector<uint8_t> bytes = {0xb8,0x78,0x56,0x34,0x12,
                0xa3,0,1,0,0, 0xff,0x25,0,1,0,0};
            for (auto c : {&ref, &cpu}) {
                c->s[CS].big = true; c->markCode(0x100);
                std::copy(bytes.begin(), bytes.end(), c->mem.begin());
            }
            Jit jit(cpu, true, 0);
            ref.step(); ref.step(); check(jit.run() == 2, "code write must end prefix before jump");
            equal(ref, cpu);
            ref.step(); check(jit.run() == 1, "jump after code invalidation"); equal(ref, cpu);
            check(cpu.ip == 0x12345678 && jit.nativeInstructions == 3, "updated code pointer target");
        }
        // A pointer page-table A-bit update can invalidate this very code page.
        // The successful helper's generation exit must retain the dynamic IP.
        for (unsigned width : {2u, 4u}) {
            CPU ref(2 * 1024 * 1024), cpu(2 * 1024 * 1024);
            auto bytes = instruction(true, width, true, -1, true);
            for (auto c : {&ref, &cpu}) {
                c->s[CS].big = true; c->s[CS].selector = 3;
                c->s[FS].base = 0x5000; c->ip = 0x100;
                c->write(0x80000, 0x81007, 4); c->write(0x81000, 0x81007, 4);
                c->write(0x81024, 0x9007, 4); c->write(0x9000, 0x12345678, width);
                std::copy(bytes.begin(), bytes.end(), c->mem.begin() + 0x81100);
                c->markCode(0x81024);
                c->cr[0] = 0x80000011; c->cr[3] = 0x80000;
            }
            Jit jit(cpu, true, 0); ref.step(); ref.step();
            check(jit.run() == 2 && jit.nativeInstructions == 2, "pointer A-bit invalidation native prefix");
            equal(ref, cpu);
            check(cpu.generation == ref.generation && cpu.generation[0x81] == 1,
                  "pointer A-bit must invalidate the executing code page once");
            check(cpu.ip == (width == 2 ? 0x5678u : 0x12345678u),
                  "successful generation exit overwrote dynamic target");
        }
        std::cout << checks << " indirect jump checks passed\n";
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
