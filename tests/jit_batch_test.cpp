#include "jit.hpp"
#include "machine.hpp"
#include <algorithm>
#include <iostream>
#include <memory>
using namespace pc;
static unsigned checks;
static bool linksMode;
static void check(bool v, const char *s) {
    ++checks;
    if (!v)
        throw std::runtime_error(s);
}
static void equal(const CPU &a, const CPU &b) {
    check(a.ip == b.ip && a.flags == b.flags && a.instructions == b.instructions &&
              a.halted == b.halted,
          "batch control state");
    check(std::equal(std::begin(a.r), std::end(a.r), std::begin(b.r)), "batch registers");
    check(a.mem == b.mem && a.cr[2] == b.cr[2], "batch RAM/CR2");
}
static void paging(CPU &c) {
    c.write(0x80000, 0x81007, 4);
    for (unsigned i = 0; i < 256; ++i)
        c.write(0x81000 + i * 4, (i << 12) | 7, 4);
    c.cr[3] = 0x80000;
    c.cr[0] |= 0x80000000;
    c.flushTLB();
}
static void timerTests(bool shortLoop = false, bool memoryLoop = false) {
    for (bool batched : {false, true})
        for (unsigned phase : {0u, 1u, 31u, 63u, 127u}) {
            Machine m("", "", 1024 * 1024);
            auto &c = m.cpu;
            c.cr[0] = 0x10;
            c.flags = 0x202;
            c.s[CS].base = 0;
            c.s[CS].selector = 0;
            c.ip = 0x1000;
            c.s[SS].base = 0;
            c.r[SP] = 0x9000;
            c.r[CX] = 10000;
            c.instructions = phase;
            c.r[BX] = 0x4000;
            const std::vector<uint8_t> loop = memoryLoop
                ? std::vector<uint8_t>{0x83, 0x07, 1, 0x83, 0xe9, 1, 0x75, 0xf8, 0xf4}
                : shortLoop
                ? std::vector<uint8_t>{0x83, 0xc0, 1, 0xf8, 0x83, 0xe9, 1, 0x75, 0xf7, 0xf4}
                : std::vector<uint8_t>{0x83, 0xc0, 1, 0x83, 0xe9, 1, 0x75, 0xf8, 0xf4};
            std::copy(std::begin(loop), std::end(loop), c.mem.begin() + 0x1000);
            const uint8_t isr[] = {0xff, 0x06, 0, 0x30, 0x50, 0xb0, 0x20, 0xe6, 0x20, 0x58, 0xcf};
            std::copy(std::begin(isr), std::end(isr), c.mem.begin() + 0x2000);
            c.write(8 * 4, 0x2000, 2);
            c.write(8 * 4 + 2, 0, 2);
            c.write(0x3000, 0, 2);
            m.out(0x43, 0x34, 1);
            m.out(0x40, 0xe8, 1);
            m.out(0x40, 3, 1);
            Jit jit(c, true, shortLoop ? 8 : 0, 65536);
            jit.setDirectLinks(linksMode, 0);
            unsigned delivered = 0;
            uint64_t worst = 0;
            while (!c.halted && c.instructions < 100000) {
                if (batched) {
                    auto r = jit.runBatch();
                    check(r.instructions && r.instructions <= 127, "unbounded board batch");
                } else
                    jit.run();
                auto deadline = (m.pit.counters[0].loadedAt +
                                 (m.pit.counters[0].lastEdge + 1) * m.pit.counters[0].reload) *
                                4;
                auto oldIp = c.ip;
                m.poll();
                if (c.ip == 0x2000 && oldIp != 0x2000) {
                    ++delivered;
                    check(c.instructions >= deadline, "timer delivered before deadline");
                    worst = std::max(worst, c.instructions - deadline);
                    check(c.instructions - deadline <= 127, "timer latency exceeds batch bound");
                }
            }
            check(c.halted && c.r[CX] == 0 && c.r[AX] == (memoryLoop ? 0u : 10000u) && c.r[SP] == 0x9000,
                  "timer ISR failed to preserve execution");
            if (memoryLoop) check(c.read(0x4000, 2) == 10000, "timer loop lost a native RMW update");
            check(delivered >= 7 && c.read(0x3000, 2) == delivered,
                  "timer EOI/IRET delivery mismatch");
            if (shortLoop || memoryLoop)
                check(jit.nativeInstructions > 25000,
                      "timer test did not execute its short/RMW ADD natively");
            std::cout << "{\"batched\":" << batched << ",\"phase\":" << phase
                      << ",\"shortLoop\":" << shortLoop
                      << ",\"memoryLoop\":" << memoryLoop
                      << ",\"instructions\":" << c.instructions << ",\"interrupts\":" << delivered
                      << ",\"worstLatencyInstructions\":" << worst << "}\n";
        }
}
int main() {
    try {
        for (auto mode :
             {Jit::BlockMode::Basic, Jit::BlockMode::FollowJumps, Jit::BlockMode::Trace})
            for (bool wide : {false, true})
                for (bool paged : {false, true})
                    for (unsigned warmup : {0u, 8u})
                        for (unsigned budget : {0u, 1u, 2u, 8u, 64u, 1000u}) {
                            CPU c(1024 * 1024);
                            c.cr[0] = 0x11;
                            c.s[CS].big = wide;
                            c.r[CX] = 1000;
                            const uint8_t code[] = {0x83, 0xc0, 1, 0x83, 0xe9, 1, 0x75, 0xf8, 0xf4};
                            std::copy(std::begin(code), std::end(code), c.mem.begin());
                            if (paged)
                                paging(c);
                            CPU ref = c;
                            Jit jit(c, true, warmup, 65536, mode);
            jit.setDirectLinks(linksMode, 0);
                            auto r = jit.runBatch(budget);
                            for (unsigned i = 0; i < r.instructions; ++i)
                                ref.step();
                            equal(c, ref);
                            check(r.instructions <= 127 && r.dispatches <= 64,
                                  "unbounded native batch");
                            check((r.dispatches == 0) == (budget == 0), "zero budget dispatched");
                            if (budget)
                                check(r.instructions > 0, "no progress");
                            if (warmup && budget)
                                check(r.instructions == 1 && r.dispatches == 1,
                                      "warming interpreter fragment batched");
                        }
        // WAIT remains an interpreter boundary; CMC and CLI now lower natively.
        for (uint8_t barrier : {uint8_t(0x9b), uint8_t(0xf4)}) {
            CPU c(65536);
            c.cr[0] = 0x11;
            c.s[CS].big = true;
            c.flags = 0x202;
            const uint8_t code[] = {0x83, 0xc0, 1, 0xeb, 0, barrier, 0x83, 0xc0, 1, 0xeb, 0};
            std::copy(std::begin(code), std::end(code), c.mem.begin());
            CPU ref = c;
            for (unsigned i = 0; i < 3; ++i)
                ref.step();
            Jit jit(c, true, 0, 65536);
            jit.setDirectLinks(linksMode, 0);
            auto r = jit.runBatch();
            equal(c, ref);
            check(r.instructions == 3 && r.dispatches == 2 && jit.nativeInstructions == 2,
                  "interpreter/HLT boundary failed to stop batching");
        }
        {
            CPU c(1024 * 1024);
            c.s[CS].big = true;
            const uint8_t code[] = {0x83, 0xc0, 1, 0x8b, 0x1d, 0, 0, 0x0a, 0, 0xeb, 0};
            std::copy(std::begin(code), std::end(code), c.mem.begin());
            unsigned reads = 0;
            c.videoRead = [&](uint32_t) {
                ++reads;
                return uint8_t(0x42);
            };
            CPU ref = c;
            ref.videoRead = [](uint32_t) { return uint8_t(0x42); };
            ref.step();
            ref.step();
            Jit jit(c, true, 0, 65536);
            jit.setDirectLinks(linksMode, 0);
            auto r = jit.runBatch();
            equal(c, ref);
            check(r.instructions == 2 && r.dispatches == 1 && reads == 4,
                  "MMIO replay batching/callback count");
        }
        for (bool trap : {false, true}) {
            CPU c(65536);
            c.s[CS].big = true;
            const uint8_t code[] = {0x83, 0xc0, 1, 0xeb, 0};
            std::copy(std::begin(code), std::end(code), c.mem.begin());
            CPU ref = c;
            c.interruptBlockedThrough = ref.interruptBlockedThrough = trap ? 0 : 1;
            if (trap)
                c.flags = ref.flags = c.flags | TF;
            ref.step();
            Jit jit(c, true, 0, 65536);
            jit.setDirectLinks(linksMode, 0);
            auto r = jit.runBatch();
            equal(c, ref);
            check(r.instructions == 1 && r.dispatches == 1 && !jit.nativeInstructions,
                  "interrupt-shadow/TF fallback was batched");
        }
        {
            CPU seed(1024 * 1024);
            seed.cr[0] = 0x11;
            seed.s[CS].big = true;
            const uint8_t code[] = {0x83, 0xc0, 1, 0xeb, 0, 0x8b, 0x1d, 0, 0x40, 0, 0};
            std::copy(std::begin(code), std::end(code), seed.mem.begin());
            paging(seed);
            seed.write(0x81010, 0, 4);
            seed.flushTLB();
            CPU a = seed, b = seed;
            struct Stop {};
            unsigned fa = 0, fb = 0;
            a.observeFault = [&](const Fault &f) {
                check(f.vector == 14, "reference fault");
                ++fa;
                throw Stop{};
            };
            b.observeFault = [&](const Fault &f) {
                check(f.vector == 14, "batch fault");
                ++fb;
                throw Stop{};
            };
            try {
                for (unsigned i = 0; i < 3; ++i)
                    a.step();
            } catch (const Stop &) {
            }
            Jit jit(b, true, 0, 65536);
            jit.setDirectLinks(linksMode, 0);
            try {
                jit.runBatch();
            } catch (const Stop &) {
            }
            equal(a, b);
            check(fa == 1 && fb == 1 && b.ip == 5 && b.cr[2] == 0x4000,
                  "precise fault after batched native block");
        }
        {
            CPU c(1024 * 1024);
            c.s[CS].big = true;
            for (unsigned n = 0; n < 512; ++n) {
                auto a = n * 32;
                c.mem[a] = 0x83;
                c.mem[a + 1] = 0xc0;
                c.mem[a + 2] = 1;
                c.mem[a + 3] = 0xe9;
                c.write(a + 4, 24, 4);
            }
            c.mem[512 * 32] = 0xf4;
            CPU ref = c;
            Jit jit(c, true, 0, 65536);
            jit.setDirectLinks(linksMode, 0);
            while (!c.halted) {
                auto r = jit.runBatch();
                check(r.instructions && r.instructions <= 127, "recycle batch progress/bound");
                for (unsigned i = 0; i < r.instructions; ++i)
                    ref.step();
                equal(c, ref);
            }
            check(c.r[AX] == 512 && jit.cacheRecycles > 0, "batch cache recycling coverage");
        }
        for (bool enabled : {false, true}) {
            linksMode = enabled;
            timerTests(); timerTests(true); timerTests(false, true);
        }
        std::cout << "Passed " << checks << " native batch checks\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
