#include "jit.hpp"
#include <algorithm>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <unistd.h>
using namespace pc;
static unsigned checks;
static bool linksMode;
static uint64_t observedLinks;
struct LinkProbe {
    Jit &jit;
    ~LinkProbe() { observedLinks += jit.directLinkTransitions; }
};
static uint64_t multiDispatchBatches;
void check(bool v, const char *s) {
    ++checks;
    if (!v)
        throw std::runtime_error(s);
}
void equal(const CPU &a, const CPU &b) {
    check(a.ip == b.ip && a.flags == b.flags && a.instructions == b.instructions &&
              a.halted == b.halted,
          "control");
    check(std::equal(a.r, a.r + 8, b.r), "GPR");
    check(a.mem == b.mem && a.cr[2] == b.cr[2], "RAM/CR2");
}
CPU make(std::initializer_list<uint8_t> code, bool paging = false) {
    CPU c(1024 * 1024);
    c.cr[0] = 0x11;
    c.s[CS].big = true;
    c.flags = 0x8d7;
    std::copy(code.begin(), code.end(), c.mem.begin());
    if (paging) {
        c.write(0x80000, 0x81007, 4);
        for (unsigned i = 0; i < 256; ++i)
            c.write(0x81000 + 4 * i, (i << 12) | 7, 4);
        c.cr[3] = 0x80000;
        c.cr[0] |= 0x80000000;
        c.flushTLB();
    }
    return c;
}
void compare(CPU c, unsigned batches = 10000, Jit::BlockMode mode = Jit::BlockMode::FollowJumps) {
    CPU ref = c, single = c;
    Jit jit(c, true, 0, 65536, mode), one(single, true, 0, 65536, mode);
        jit.setDirectLinks(linksMode, 0);
        LinkProbe linkProbe{jit};
    for (unsigned k = 0; k < batches && !c.halted; ++k) {
        auto p = jit.runBatch();
        Jit::BatchResult q;
        auto native = one.nativeInstructions;
        while (q.instructions < 64 && !single.halted) {
            auto n = one.run();
            q.instructions += n;
            ++q.dispatches;
            if (!n || one.nativeInstructions - native != q.instructions)
                break;
        }
        check(p.instructions == q.instructions && p.dispatches == q.dispatches, "calendar");
        for (unsigned i = 0; i < p.instructions; ++i)
            ref.step();
        equal(c, ref);
        equal(c, single);
    }
}

static void memoryTests() {

    for (bool paging : {false, true})
        for (unsigned loops : {1u, 3u, 22u, 64u, 1000u}) {
            auto c = make({0x8b, 0x03, 0x83, 0xc3, 4, 0x83, 0xe9, 1, 0x75, 0xf6, 0xf4}, paging);
            c.r[BX] = 0x10000;
            c.r[CX] = loops;
            for (unsigned i = 0; i < loops; ++i)
                c.write(0x10000 + 4 * i, i * 17, 4);
            compare(c);
        }
    for (bool paging : {false, true}) {
        auto c = make({0x89, 0x03, 0x83, 0xeb, 8, 0x83, 0xe9, 1, 0x75, 0xf6, 0xf4}, paging);
        c.r[BX] = 0x20;
        c.r[CX] = 100;
        c.r[AX] = 0x90f4fe75;
        compare(c, 4);
    }
    {
        auto c = make({0x8b, 0x03, 0x83, 0xc3, 4, 0x83, 0xe9, 1, 0x75, 0xf6, 0xf4});
        c.r[BX] = 0x9fff0;
        c.r[CX] = 100;
        CPU ref = c;
        unsigned callbacks = 0, refCallbacks = 0;
        c.videoRead = [&](uint32_t) {
            ++callbacks;
            check(c.instructions == 17, "MMIO clock before replay");
            return 0x42;
        };
        ref.videoRead = [&](uint32_t) {
            ++refCallbacks;
            return 0x42;
        };
        Jit jit(c, true, 0, 65536, Jit::BlockMode::FollowJumps);
        jit.setDirectLinks(linksMode, 0);
        LinkProbe linkProbe{jit};
        auto p = jit.runBatch();
        check(p.instructions == 17 && p.dispatches == 5, "MMIO prefix/calendar");
        for (unsigned i = 0; i < p.instructions; ++i)
            ref.step();
        equal(c, ref);
        check(callbacks == 4 && refCallbacks == 4, "MMIO exactly once");
        check(jit.cacheHits == 5 && jit.nativeInstructions == 16, "MMIO accumulated native prefix");
    }
    // A fault after eight complete iterations must expose the original restart state.
    {
        auto c = make({0x8b, 0x03, 0x83, 0xc3, 4, 0x83, 0xe9, 1, 0x75, 0xf6, 0xf4}, true);
        c.r[BX] = 0x4fe0;
        c.r[CX] = 16;
        c.s[CS].selector = 3;
        c.write(0x81000 + 5 * 4, 0, 4);
        c.flushTLB();
        for (unsigned i = 0; i < 8; ++i)
            c.write(0x4fe0 + 4 * i, 100 + i, 4);
        CPU ref = c;
        struct Stop {};
        unsigned faults = 0, refFaults = 0;
        c.observeFault = [&](const Fault &f) {
            ++faults;
            check(f.vector == 14 && f.error == 4, "native fault type");
            throw Stop{};
        };
        ref.observeFault = [&](const Fault &f) {
            ++refFaults;
            check(f.vector == 14 && f.error == 4, "interpreter fault type");
            throw Stop{};
        };
        Jit jit(c, true, 0, 65536, Jit::BlockMode::FollowJumps);
        jit.setDirectLinks(linksMode, 0);
        LinkProbe linkProbe{jit};
        try {
            jit.runBatch();
            check(false, "missing native fault");
        } catch (const Stop &) {
        }
        try {
            for (unsigned i = 0; i < 33; ++i)
                ref.step();
            check(false, "missing interpreter fault");
        } catch (const Stop &) {
        }
        equal(c, ref);
        check(faults == 1 && refFaults == 1 && c.instructions == 33 && c.cr[2] == 0x5000,
              "precise accumulated fault");
    }
    // A data alias replaces the code PTE and evicts its TLB slot without changing
    // the old physical code page generation. The next iteration must fetch anew.
    {
        auto c = make({0x89, 0x03, 0x83, 0xe9, 1, 0x75, 0xf9, 0xf4}, true);
        std::copy_n(c.mem.begin(), 8, c.mem.begin() + 0x2000);
        c.mem[0x2000] = 0xff;
        c.mem[0x2001] = 0xc2;
        unsigned page = 1024;
        while (((page ^ (page >> 10)) & (c.tlb.size() - 1)) != 0)
            ++page;
        c.write(0x80000 + 4 * (page >> 10), 0x82007, 4);
        c.write(0x82000 + 4 * (page & 1023), 0x81007, 4);
        c.r[BX] = page << 12;
        c.r[CX] = 4;
        c.r[AX] = 0x2007;
        c.flushTLB();
        CPU ref = c;
        Jit jit(c, true, 0, 65536, Jit::BlockMode::FollowJumps);
        jit.setDirectLinks(linksMode, 0);
        LinkProbe linkProbe{jit};
        auto n = jit.runBatch();
        for (unsigned i = 0; i < n.instructions; ++i)
            ref.step();
        equal(c, ref);
        check(c.halted && c.r[DX] == 3 && c.instructions == 13, "code remap followed");
    }
}

static void cycleTests() {

    for (auto mode : {Jit::BlockMode::Basic, Jit::BlockMode::FollowJumps, Jit::BlockMode::Trace})
        for (bool paging : {false, true})
            for (bool wide : {false, true})
                for (bool memory : {false, true})
                    for (unsigned warm : {0u, 8u})
                        for (unsigned loops : {1u, 3u, 22u, 64u, 1000u}) {
                            CPU c(1024 * 1024);
                            c.cr[0] = 0x11;
                            c.s[CS].big = wide;
                            c.r[CX] = loops;
                            c.r[BX] = 0x1000;
                            c.flags = 0x8d7;
                            std::vector<uint8_t> code(32, 0x90);
                            std::vector<uint8_t> first =
                                memory
                                    ? std::vector<uint8_t>{0x8b, 0x07, 0x85, 0xc9, 0x75, 0x0a, 0xf4}
                                    : std::vector<uint8_t>{0x83, 0xc0, 1, 0x85,
                                                           0xc9, 0x75, 9, 0xf4};
                            // 16-bit addressing uses [BX] via ModRM 07, 32-bit uses [EBX] via 03.
                            if (memory && wide)
                                first[1] = 3;
                            std::copy(first.begin(), first.end(), code.begin());
                            std::vector<uint8_t> second =
                                memory ? std::vector<uint8_t>{0x83, 0xc3, 4,    0x83, 0xe9,
                                                              1,    0x75, 0xe8, 0xf4}
                                       : std::vector<uint8_t>{0x83, 0xe9, 1, 0x75, 0xeb, 0xf4};
                            std::copy(second.begin(), second.end(), code.begin() + 16);
                            std::copy(code.begin(), code.end(), c.mem.begin());
                            for (unsigned i = 0; i < loops; ++i)
                                c.write(0x1000 + 4 * i, 17 * i, 4);
                            if (paging) {
                                c.write(0x80000, 0x81007, 4);
                                for (unsigned i = 0; i < 256; ++i)
                                    c.write(0x81000 + 4 * i, (i << 12) | 7, 4);
                                c.cr[3] = 0x80000;
                                c.cr[0] |= 0x80000000;
                                c.flushTLB();
                            }
                            CPU ref = c, single = c;
                            Jit jit(c, true, warm, 65536, mode),
                                one(single, true, warm, 65536, mode);
        jit.setDirectLinks(linksMode, 0);
        LinkProbe linkProbe{jit};
                            unsigned batches = 0;
                            while (!c.halted) {
                                auto part = jit.runBatch();
                                Jit::BatchResult expected;
                                auto native = one.nativeInstructions;
                                while (expected.instructions < 64 && !single.halted) {
                                    auto n = one.run();
                                    expected.instructions += n;
                                    ++expected.dispatches;
                                    if (!n ||
                                        one.nativeInstructions - native != expected.instructions)
                                        break;
                                }
                                check(++batches < 10000, "chain hang");
                                check(part.instructions == expected.instructions &&
                                          part.dispatches == expected.dispatches,
                                      "chain dispatch calendar");
                                check(part.instructions && part.instructions <= 127,
                                      "chain budget");
                                for (unsigned i = 0; i < part.instructions; ++i)
                                    ref.step();
                                equal(c, ref);
                                equal(c, single);
                                if (part.dispatches > 1)
                                    ++multiDispatchBatches;
                            }
                        }
    check(multiDispatchBatches > 0, "no multi-dispatch batch exercised");
}

static void debugMapTests() {
    auto folder = std::filesystem::temp_directory_path() /
                  ("pcemu-jit-debug-" + std::to_string(getpid()));
    std::filesystem::create_directory(folder);
    auto path = folder / "map.jsonl";
    auto c = make({0xb8, 1, 0, 0, 0, 0x83, 0xc0, 2, 0xf4});
    CPU ref = c;
    Jit jit(c, true, 0);
        jit.setDirectLinks(linksMode, 0);
        LinkProbe linkProbe{jit};
    jit.setDebugMap(path.string());
    bool rejected = false;
    try { jit.setDebugMap(path.string()); } catch (const std::runtime_error &) { rejected = true; }
    check(rejected, "debug map overwrite accepted");
    for (unsigned run = 0; run < 2; ++run) {
        c.ip = ref.ip = 0;
        c.halted = ref.halted = false;
        auto batch = jit.runBatch();
        for (unsigned i = 0; i < batch.instructions; ++i) ref.step();
        equal(c, ref);
        jit.clear(); // Start a new address lifetime before reusing the arena.
    }
    jit.flushDebugMap();
    std::ifstream file(path);
    std::string text((std::istreambuf_iterator<char>(file)), {});
    check(text.find("\"arm64\":") != std::string::npos, "debug machine bytes missing");
    check(text.find("\"generation\":3") != std::string::npos, "debug reset lifetime missing");
    check(text.find("\"guestIP\":5") != std::string::npos, "debug instruction start missing");
    file.close();
    std::filesystem::remove(path);
    std::filesystem::remove(folder);
}

static void linkedContextTests() {
    // ADC's carry spill uses offset 104. It must not overwrite chain budget or
    // packed totals; that collision was visible in Word's kernel startup path.
    {
        auto c = make({0x83, 0xd0, 1, 0x75, 0xfb, 0xf4});
        c.r[AX] = 0xfffffffc;
        c.flags = 2;
        CPU ref = c;
        Jit jit(c, true, 0, 65536);
        jit.setDirectLinks(true, 0);
        auto part = jit.runBatch();
        for (unsigned i = 0; i < part.instructions; ++i) ref.step();
        equal(c, ref);
        check(c.halted && part.instructions == 9 && part.dispatches == 5,
              "carry spill changed linked accounting");
        check(jit.directLinkTransitions >= 2, "carry loop did not use direct edges");
    }
    for (bool paged : {false, true})
        for (unsigned mutation = 0; mutation < 6; ++mutation) {
            auto c = make({0x83, 0xc0, 1, 0x83, 0xe9, 1, 0x75, 0xf8, 0xf4}, paged);
            c.r[CX] = 10000;
            CPU ref = c;
            Jit jit(c, true, 0, 65536);
            jit.setDirectLinks(true, 0);
            auto advance = [&]() {
                auto part = jit.runBatch();
                check(part.instructions && part.instructions <= 127, "linked context budget");
                for (unsigned i = 0; i < part.instructions; ++i) ref.step();
                equal(c, ref);
            };
            advance();
            check(jit.directLinkTransitions > 0, "context mutation lacks live direct edges");
            if (mutation == 0) {
                auto replacement = c.generation;
                c.generation.swap(replacement);
            } else if (mutation == 1) {
                auto replacement = c.mem;
                c.mem.swap(replacement);
            } else if (mutation == 2) {
                ++c.executionEpoch;
            } else if (mutation == 3) {
                c.a20 = ref.a20 = false;
            } else if (mutation == 4) {
                c.write(2, 2, 1);
                ref.write(2, 2, 1);
            } else {
                c.flushTLB();
                ref.flushTLB();
            }
            for (unsigned i = 0; i < 8; ++i) advance();
        }
}

static void stringMoveTests() {
    for (bool paging : {false, true})
        for (bool wide : {false, true})
            for (bool address32 : {false, true})
                for (unsigned width : {1u, 2u, 4u})
                    for (bool backwards : {false, true})
                      for (bool store : {false, true}) {
                        auto c = make({}, paging);
                        c.s[CS].big = wide;
                        std::vector<uint8_t> code{0x64}; // FS override
                        if (wide != address32) code.push_back(0x67);
                        if (width != 1 && wide != (width == 4)) code.push_back(0x66);
                        code.push_back(store ? (width == 1 ? 0xaa : 0xab) : (width == 1 ? 0xac : 0xad));
                        code.push_back(0xf4);
                        std::copy(code.begin(), code.end(), c.mem.begin());
                        c.s[FS].base = 0x2000;
                        c.s[ES].base = 0x3000; // STOS ignores the FS override.
                        c.r[SI] = address32 ? 0x100 : 0xabcd0100;
                        c.r[DI] = c.r[SI];
                        c.r[AX] = 0x87654321;
                        c.flags = 0x8d7 | (backwards ? DF : 0);
                        c.write(0x2100, 0x12345678, 4);
                        compare(c);
                    }
    {
        auto c = make({0xfd, 0xac, 0xfc, 0xad, 0xaa, 0xab, 0xf4}, true);
        c.r[SI] = 0x11000; c.r[DI] = 0x12000;
        c.flags = 0x8d7;
        compare(c); // Both DF changes execute inside the same native block.
    }
    {
        auto c = make({0xab, 0xf4, 0x90, 0x90, 0x90});
        c.r[DI] = 1; c.r[AX] = 0x909090f4;
        compare(c); // Self-modifying STOS must commit DI before leaving the block.
    }
    {
        auto c = make({0xab, 0xf4});
        c.r[DI] = 0xa0000;
        c.r[AX] = 0x12345678;
        CPU ref = c;
        unsigned calls = 0, refCalls = 0;
        c.videoWrite = [&](uint32_t, uint8_t) { ++calls; };
        ref.videoWrite = [&](uint32_t, uint8_t) { ++refCalls; };
        Jit jit(c, true, 0);
        jit.setDirectLinks(linksMode, 0);
        while (!c.halted) {
            auto part = jit.runBatch();
            for (unsigned i = 0; i < part.instructions; ++i) ref.step();
            equal(c, ref);
        }
        check(calls == 4 && refCalls == 4 && c.r[DI] == 0xa0004,
              "STOS MMIO replay duplicated access or destination-index update");
    }
    for (bool backwards : {false, true}) {
        auto c = make({0xac, 0xf4});
        c.s[CS].big = false;
        c.r[SI] = backwards ? 0x12340000 : 0x1234ffff;
        c.flags = 2 | (backwards ? DF : 0);
        compare(c); // SI wraps while its high half survives.
    }
    for (bool backwards : {false, true}) {
        auto c = make({0xac, 0x83, 0xe9, 1, 0x75, 0xfa, 0xf4}, true);
        c.r[CX] = 1000;
        c.r[SI] = backwards ? 0x11000 : 0x10000;
        c.flags = 2 | (backwards ? DF : 0);
        compare(c);
    }
    {
        auto c = make({0xf3, 0xac, 0xf4});
        c.r[SI] = 0x1000;
        c.r[CX] = 3;
        compare(c); // REP remains interpreted and must retain its count semantics.
    }
    {
        auto c = make({0xac, 0xf4});
        c.r[SI] = 0x1000;
        CPU ref = c;
        Jit jit(c, true, 8);
        jit.setDirectLinks(linksMode, 0);
        for (unsigned run = 0; run < 40; ++run) {
            c.ip = ref.ip = 0;
            c.halted = ref.halted = false;
            while (!c.halted) {
                auto part = jit.runBatch();
                for (unsigned i = 0; i < part.instructions; ++i) ref.step();
                equal(c, ref);
            }
        }
        check(jit.nativeInstructions > 0, "hot single LODS was rejected as a cheap fragment");
    }
    {
        auto c = make({0xad, 0xf4});
        c.r[SI] = 0xa0000;
        CPU ref = c;
        unsigned calls = 0, refCalls = 0;
        c.videoRead = [&](uint32_t p) { ++calls; return uint8_t(p); };
        ref.videoRead = [&](uint32_t p) { ++refCalls; return uint8_t(p); };
        Jit jit(c, true, 0);
        jit.setDirectLinks(linksMode, 0);
        while (!c.halted) {
            auto part = jit.runBatch();
            for (unsigned i = 0; i < part.instructions; ++i) ref.step();
            equal(c, ref);
        }
        check(calls == 4 && refCalls == 4 && c.r[SI] == 0xa0004,
              "LODS MMIO replay duplicated access or source-index update");
    }
    for (bool store : {false, true}) {
        auto c = make({0xac, uint8_t(store ? 0xab : 0xad), 0xf4}, true);
        c.r[SI] = 0x4fff;
        c.r[DI] = 0x4fff;
        c.mem[0x4fff] = 0x78;
        uint32_t absent = 0;
        std::copy_n(reinterpret_cast<uint8_t *>(&absent), 4, c.mem.begin() + 0x81014);
        c.flushTLB();
        CPU ref = c;
        unsigned faults = 0;
        auto capture = [&](const Fault &f) {
            check(f.vector == 14 && f.error == (store ? 2u : 0u), "string access fault type");
            ++faults;
            throw std::runtime_error("captured LODS fault");
        };
        c.observeFault = ref.observeFault = capture;
        Jit jit(c, true, 0);
        jit.setDirectLinks(linksMode, 0);
        try { jit.runBatch(); check(false, "LODS native fault missing"); }
        catch (const std::runtime_error &e) { check(std::string(e.what()) == "captured LODS fault", "LODS fault observer"); }
        try { ref.step(); ref.step(); check(false, "LODS reference fault missing"); }
        catch (const std::runtime_error &e) { check(std::string(e.what()) == "captured LODS fault", "LODS reference observer"); }
        equal(c, ref);
        check(faults == 2 && c.r[SI] == 0x5000 && c.r[DI] == 0x4fff &&
                  c.ip == 1 && c.cr[2] == 0x5000,
              "string fault lost the successful prefix or changed an index");
    }
}

static void memoryBitStringTests() {
    auto append = [](std::vector<uint8_t> &code, uint32_t value, unsigned width) {
        for (unsigned b = 0; b < width; ++b) code.push_back(uint8_t(value >> (b * 8)));
    };
    for (bool paging : {false, true})
     for (bool code32 : {false, true})
      for (bool address32 : {false, true})
       for (unsigned width : {2u, 4u})
        for (unsigned operation : {4u, 5u, 6u, 7u})
         for (bool immediate : {false, true})
          for (int index : {-32768, -65, -33, -16, -1, 0, 1, 15, 16, 31, 32, 65, 255, 32767}) {
            auto c = make({}, paging);
            c.s[CS].big = code32; c.s[FS].base = 0x3000;
            c.r[BX] = address32 ? 0xffee : 0x1234ffee;
            c.r[SI] = 0xabcd0010; c.r[DX] = uint32_t(index);
            std::vector<uint8_t> code{0x64};
            if (code32 != address32) code.push_back(0x67);
            if (code32 != (width == 4)) code.push_back(0x66);
            code.push_back(0x0f);
            code.push_back(immediate ? 0xba : operation == 4 ? 0xa3 : operation == 5 ? 0xab : operation == 6 ? 0xb3 : 0xbb);
            code.push_back(uint8_t(0x40 | ((immediate ? operation : 2) << 3) | (address32 ? 4 : 0)));
            if (address32) { code.push_back(0x33); c.r[SI] = 0x10; } // BX + SI
            code.push_back(0x10); // Base offset wraps at 16 bits before index adjustment.
            if (immediate) code.push_back(uint8_t(index));
            code.push_back(0x9f); code.push_back(0xf4);
            std::copy(code.begin(), code.end(), c.mem.begin());
            // Deterministic data on either side of both effective-address regions.
            for (uint32_t a : {0x2f00u, 0x12f00u})
                for (unsigned n = 0; n < 512; ++n) c.mem[a + n] = uint8_t(n * 37 + 0xa5);
            compare(c);
        }
    for (unsigned operation : {4u, 5u, 6u, 7u}) {
        auto c = make({0x0f, uint8_t(operation == 4 ? 0xa3 : operation == 5 ? 0xab : operation == 6 ? 0xb3 : 0xbb), 0x13, 0xf4}, true);
        c.r[BX] = 0x1000; c.r[DX] = 33;
        CPU ref = c; Jit jit(c, true, 0); jit.setDirectLinks(linksMode, 0);
        auto part = jit.runBatch();
        for (unsigned i = 0; i < part.instructions; ++i) ref.step();
        equal(c, ref);
        check(jit.nativeInstructions == 1 && c.halted, "memory bit string did not execute natively");
    }
    for (unsigned operation : {4u, 5u, 6u, 7u}) {
        auto opcode = uint8_t(operation == 4 ? 0xa3 : operation == 5 ? 0xab : operation == 6 ? 0xb3 : 0xbb);
        auto c = make({0x83, 0xc0, 1, 0x0f, opcode, 0x13, 0x83, 0xd1, 0, 0x9f, 0xf4}, true);
        c.r[BX] = 0x1000; c.r[DX] = 33; c.r[AX] = 0x7fffffff;
        c.write(0x1004, 2, 4);
        compare(c); // Pending flags -> CF-only update -> carry consumer.
        for (auto mode : {Jit::BlockMode::Basic, Jit::BlockMode::FollowJumps, Jit::BlockMode::Trace}) {
            auto loop = make({0x0f, opcode, 0x13, 0x83, 0xe9, 1, 0x75, 0xf8, 0xf4}, true);
            loop.r[BX] = 0x1000; loop.r[DX] = 33; loop.r[CX] = 1000;
            compare(loop, 10000, mode);
        }
    }
    // An index that aliases an address register is captured before address adjustment.
    compare(make({0xbb, 0x01, 0x10, 0, 0, 0x0f, 0xab, 0x1b, 0x0f, 0xa3, 0x1b, 0xf4}, true));
    for (unsigned operation : {5u, 6u, 7u}) {
        auto c = make({0x0f, uint8_t(operation == 5 ? 0xab : operation == 6 ? 0xb3 : 0xbb), 0x13, 0xf4}, true);
        uint32_t selfPte = 0x81007;
        std::copy_n(reinterpret_cast<uint8_t *>(&selfPte), 4, c.mem.begin() + 0x81024);
        c.flushTLB(); c.r[BX] = 0x9024; c.r[DX] = 6;
        compare(c); // CF must observe D before this access's write probe sets it.
    }
    {
        auto c = make({0x0f, 0xab, 0x13, 0x74, 0x03, 0x83, 0xc1, 1, 0xf4});
        c.r[BX] = 3; c.r[DX] = 7;
        compare(c); // BTS changes the next opcode from JZ to HLT.
    }
    for (unsigned operation : {4u, 5u, 6u, 7u})
     for (bool watched : {false, true}) {
        auto c = make({0x83, 0xc0, 1, 0x0f, uint8_t(operation == 4 ? 0xa3 : operation == 5 ? 0xab : operation == 6 ? 0xb3 : 0xbb), 0x13, 0xf4});
        c.r[BX] = 0x9fffc; c.r[DX] = 38; c.r[AX] = 0x7fffffff;
        CPU ref = c;
        std::vector<uint64_t> trace, refTrace;
        c.videoRead = [&](uint32_t p) { trace.push_back(p); return uint8_t(p + 0x40); };
        ref.videoRead = [&](uint32_t p) { refTrace.push_back(p); return uint8_t(p + 0x40); };
        c.videoWrite = [&](uint32_t p, uint8_t v) { trace.push_back((1ull << 60) | (uint64_t(p) << 8) | v); };
        ref.videoWrite = [&](uint32_t p, uint8_t v) { refTrace.push_back((1ull << 60) | (uint64_t(p) << 8) | v); };
        if (watched) {
            c.watchStart = ref.watchStart = 0xa0000; c.watchLength = ref.watchLength = 4;
            c.watchWrite = [&](uint32_t, uint32_t, int) { trace.push_back((2ull << 60) | c.flags); };
            ref.watchWrite = [&](uint32_t, uint32_t, int) { refTrace.push_back((2ull << 60) | ref.flags); };
        }
        Jit jit(c, true, 0); jit.setDirectLinks(linksMode, 0);
        while (!c.halted) {
            auto part = jit.runBatch();
            for (unsigned i = 0; i < part.instructions; ++i) ref.step();
            equal(c, ref);
        }
        check(trace == refTrace && trace.size() == (operation == 4 ? 4u : 8u + unsigned(watched)),
              "memory bit-string MMIO/watch replay order");
        check(c.nativeMemoryCalls, "memory bit-string did not exercise native replay");
    }
    for (bool crossPage : {false, true})
     for (unsigned operation : {4u, 5u, 6u, 7u}) {
        auto c = make({0x83, 0xc0, 1, 0x0f, uint8_t(operation == 4 ? 0xa3 : operation == 5 ? 0xab : operation == 6 ? 0xb3 : 0xbb), 0x13, 0xf4}, true);
        c.s[CS].selector = 3; c.r[AX] = 0x7fffffff;
        c.r[BX] = crossPage ? 0x4ffb : 0x4ffc; c.r[DX] = 38;
        uint32_t pte = operation == 4 ? 0x5004 : 0x5005;
        std::copy_n(reinterpret_cast<uint8_t *>(&pte), 4, c.mem.begin() + 0x81014);
        c.mem[0x4fff] = 0x7f; c.flushTLB();
        CPU ref = c; unsigned faults = 0;
        c.observeFault = ref.observeFault = [&](const Fault &f) {
            check(f.vector == 14 && f.error == (operation == 4 ? 4u : 7u), "bit-string fault type");
            ++faults; throw std::runtime_error("bit fault");
        };
        Jit jit(c, true, 0); jit.setDirectLinks(linksMode, 0);
        try { jit.runBatch(); check(false, "missing native bit fault"); }
        catch (const std::runtime_error &e) { check(std::string(e.what()) == "bit fault", "native bit fault"); }
        try { ref.step(); ref.step(); check(false, "missing reference bit fault"); }
        catch (const std::runtime_error &e) { check(std::string(e.what()) == "bit fault", "reference bit fault"); }
        equal(c, ref);
        check(faults == 2 && c.ip == 3 && c.cr[2] == 0x5000 && c.mem[0x4fff] == 0x7f,
              "bit-string fault lost restart flags or committed data");
    }
}

static void memoryExchangeTests() {
    for (bool paging : {false, true})
      for (bool code32 : {false, true})
       for (bool address32 : {false, true})
        for (unsigned width : {1u, 2u, 4u})
         for (unsigned reg : {0u, 3u, 4u, 6u}) {
            auto c = make({}, paging);
            c.s[CS].big = code32;
            c.s[FS].base = 0x2000;
            c.r[BX] = 0x1000; c.r[SI] = 0x10;
            c.r[AX] = 0x12345678; c.r[SP] = 0x87654321;
            std::vector<uint8_t> code{0x64};
            if (code32 != address32) code.push_back(0x67);
            if (width != 1 && code32 != (width == 4)) code.push_back(0x66);
            code.push_back(width == 1 ? 0x86 : 0x87);
            code.push_back(uint8_t(0x40 | (reg << 3) | (address32 ? 4 : 0)));
            if (address32) code.push_back(0x73); // BX + SI*2 + 0x20
            code.push_back(0x20);
            code.push_back(0x9f); code.push_back(0xf4);
            std::copy(code.begin(), code.end(), c.mem.begin());
            c.write(address32 ? 0x3040 : 0x3030, 0xfedcba98, width);
            compare(c); // Includes source/address and high-byte aliases.
        }
    for (bool paging : {false, true}) {
        auto c = make({0x87, 0x03, 0xf4}, paging);
        c.r[BX] = 0x1000; c.r[AX] = 0x12345678;
        CPU ref = c;
        Jit jit(c, true, 0); jit.setDirectLinks(linksMode, 0);
        auto part = jit.runBatch();
        for (unsigned i = 0; i < part.instructions; ++i) ref.step();
        equal(c, ref);
        check(jit.nativeInstructions == 1 && c.halted, "memory XCHG did not execute natively");
    }
    {
        auto c = make({0x87, 0x03, 0xf4}, true);
        uint32_t selfPte = 0x81007;
        std::copy_n(reinterpret_cast<uint8_t *>(&selfPte), 4, c.mem.begin() + 0x81024);
        c.flushTLB(); c.r[BX] = 0x9024; c.r[AX] = 0x81067;
        compare(c); // Return the value before the write probe sets its own D bit.
    }
    {
        auto c = make({0x87, 0x03, 0x90, 0x83, 0xc1, 1, 0xf4});
        c.r[BX] = 2; c.r[AX] = 0x909090f4;
        compare(c); // Replace the next instruction with HLT.
    }
    for (bool watched : {false, true}) {
        auto c = make({0x83, 0xc0, 1, 0x87, 0x13, 0xf4});
        c.r[BX] = 0xa0000; c.r[AX] = 0x7fffffff; c.r[DX] = 0x12345678;
        CPU ref = c;
        std::vector<uint64_t> trace, refTrace;
        c.videoRead = [&](uint32_t p) { trace.push_back(p); return uint8_t(p); };
        ref.videoRead = [&](uint32_t p) { refTrace.push_back(p); return uint8_t(p); };
        c.videoWrite = [&](uint32_t p, uint8_t v) { trace.push_back((1ull << 60) | (uint64_t(p) << 8) | v); };
        ref.videoWrite = [&](uint32_t p, uint8_t v) { refTrace.push_back((1ull << 60) | (uint64_t(p) << 8) | v); };
        if (watched) {
            c.watchStart = ref.watchStart = 0xa0000;
            c.watchLength = ref.watchLength = 4;
            c.watchWrite = [&](uint32_t, uint32_t, int) { trace.push_back((2ull << 60) | c.flags); };
            ref.watchWrite = [&](uint32_t, uint32_t, int) { refTrace.push_back((2ull << 60) | ref.flags); };
        }
        Jit jit(c, true, 0); jit.setDirectLinks(linksMode, 0);
        while (!c.halted) {
            auto part = jit.runBatch();
            for (unsigned i = 0; i < part.instructions; ++i) ref.step();
            equal(c, ref);
        }
        check(trace == refTrace && trace.size() >= 8, "XCHG MMIO/watch replay order");
        check(c.nativeMemoryCalls, "XCHG MMIO did not exercise native replay");
    }
    for (bool crossPage : {false, true}) {
        auto c = make({0x83, 0xc0, 1, 0x87, 0x13, 0xf4}, true);
        c.s[CS].selector = 3; c.r[AX] = 0x7fffffff; c.r[DX] = 0x12345678;
        c.r[BX] = crossPage ? 0x4fff : 0x5000;
        uint32_t readOnly = 0x5005;
        std::copy_n(reinterpret_cast<uint8_t *>(&readOnly), 4, c.mem.begin() + 0x81014);
        c.mem[0x4fff] = 0x7f; c.flushTLB();
        CPU ref = c;
        unsigned faults = 0;
        c.observeFault = ref.observeFault = [&](const Fault &f) {
            check(f.vector == 14 && f.error == 7, "XCHG write fault type");
            ++faults; throw std::runtime_error("XCHG fault");
        };
        Jit jit(c, true, 0); jit.setDirectLinks(linksMode, 0);
        try { jit.runBatch(); check(false, "missing native XCHG fault"); }
        catch (const std::runtime_error &e) { check(std::string(e.what()) == "XCHG fault", "native XCHG fault"); }
        try { ref.step(); ref.step(); check(false, "missing reference XCHG fault"); }
        catch (const std::runtime_error &e) { check(std::string(e.what()) == "XCHG fault", "reference XCHG fault"); }
        equal(c, ref);
        check(faults == 2 && c.ip == 3 && c.r[DX] == 0x12345678 && c.mem[0x4fff] == 0x7f,
              "XCHG write fault committed a register or partial store");
    }
}

static void memoryWriteCoverageTests() {
    auto append = [](std::vector<uint8_t> &code, uint32_t value, unsigned width) {
        for (unsigned b = 0; b < width; ++b) code.push_back(uint8_t(value >> (b * 8)));
    };
    for (bool paging : {false, true})
      for (bool code32 : {false, true})
        for (bool address32 : {false, true})
          for (unsigned width : {1u, 2u, 4u})
            for (bool allOnes : {false, true})
              for (int operation : {-1, 0, 1, 2, 3, 4, 5, 6, 7})
                for (unsigned form : {0u, 1u, 2u, 3u}) {
                    if ((operation == -1 && form != 1) || (form == 2 && width == 1)) continue;
                    auto c = make({}, paging);
                    c.s[CS].big = code32;
                    c.s[FS].base = 0x2000;
                    c.r[BX] = address32 ? 0x1000 : 0x12341000;
                    c.r[SI] = address32 ? 0x10 : 0xabcd0010;
                    c.r[AX] = 0x12348100; // AH is the byte register source.
                    c.r[DX] = width == 2 ? 0x8001 : 0x80000001;
                    c.flags = 0x8d7 & ~(allOnes ? CF : 0u);
                    std::vector<uint8_t> code{0x64};
                    if (code32 != address32) code.push_back(0x67);
                    if (width != 1 && code32 != (width == 4)) code.push_back(0x66);
                    unsigned reg = form == 3 ? 4 : form == 0 ? (width == 1 ? 4 : 2) : unsigned(operation);
                    if (operation == -1) { code.push_back(width == 1 ? 0xc6 : 0xc7); reg = 0; }
                    else if (form == 0 || form == 3)
                        code.push_back(uint8_t(operation * 8 + (width != 1) + (form == 3 ? 2 : 0)));
                    else code.push_back(form == 2 ? 0x83 : width == 1 ? 0x80 : 0x81);
                    code.push_back(uint8_t(0x40 | (reg << 3) | (address32 ? 4 : 0)));
                    if (address32) code.push_back(0x73); // [BX + SI*2 + 0x20]
                    code.push_back(0x20);
                    if (operation == -1) append(code, 0x89abcdef, width);
                    else if (form == 1 || form == 2) append(code, form == 2 ? 0xff : 1, form == 2 ? 1 : width);
                    if (form == 3 && width == 1) { code.push_back(0x88); code.push_back(0xe2); }
                    code.push_back(0x9f); // Observe arithmetic flags inside the native block.
                    code.push_back(0xf4);
                    std::copy(code.begin(), code.end(), c.mem.begin());
                    c.write(address32 ? 0x3040 : 0x3030,
                            allOnes ? 0xffffffff : width == 1 ? 0x7f : width == 2 ? 0x7fff : 0x7fffffff,
                            width);
                    if (paging && allOnes) {
                        // Exercise the successful slow RMW path as well as
                        // cached dirty-page hits, including exact A/D updates.
                        uint32_t cleanPte = 0x3007;
                        std::copy_n(reinterpret_cast<uint8_t *>(&cleanPte), 4, c.mem.begin() + 0x8100c);
                        c.flushTLB();
                    }
                    compare(c);
                }
    compare(make({0xc6, 0xc4, 0x81, 0xc7, 0xc2, 0x78, 0x56, 0x34, 0x12, 0xf4}));
    for (unsigned operation : {2u, 3u})
      for (bool carry : {false, true})
        for (bool registerDestination : {false, true}) {
            auto c = registerDestination
                ? make({uint8_t(operation * 8 + 3), 0x13, 0xf4}, true)
                : make({0x83, uint8_t((operation << 3) | 3), 0, 0xf4}, true);
            c.r[BX] = 0x1000;
            c.flags = 2 | (carry ? CF : 0);
            CPU ref = c;
            Jit jit(c, true, 0);
            jit.setDirectLinks(linksMode, 0);
            auto part = jit.runBatch();
            for (unsigned i = 0; i < part.instructions; ++i) ref.step();
            equal(c, ref);
            check(jit.nativeInstructions == 1 && c.nativeMemoryCalls,
                  "carry memory operation did not execute its cold-TLB helper path");
        }
    {
        auto c = make({0x83, 0xc0, 1, 0x83, 0x13, 0, 0x1b, 0x13, 0x9f, 0xf4}, true);
        c.r[AX] = 0xffffffff; c.r[BX] = 0x1000;
        c.flags = 2;
        c.write(0x1000, 0xffffffff, 4);
        compare(c); // Pending register carry -> memory ADC -> register SBB.
    }
    for (unsigned operation : {2u, 3u})
      for (auto mode : {Jit::BlockMode::Basic, Jit::BlockMode::FollowJumps, Jit::BlockMode::Trace}) {
        auto c = make({0x83, uint8_t((operation << 3) | 3), 1,
                       0x83, 0xe9, 1, 0x75, 0xf8, 0xf4}, true);
        c.r[BX] = 0x1000; c.r[CX] = 1000;
        c.flags = 2 | CF;
        c.write(0x1000, 0xffffffff, 4);
        compare(c, 10000, mode); // Carry survives helpers, trace exits and real direct links.
    }
    {
        auto c = make({0xc7, 0x03, 0x78, 0x56, 0x34, 0x12, 0xf4}, true);
        c.r[BX] = 0x1000;
        CPU ref = c;
        Jit jit(c, true, 0);
        jit.setDirectLinks(linksMode, 0);
        auto part = jit.runBatch();
        for (unsigned i = 0; i < part.instructions; ++i) ref.step();
        equal(c, ref);
        check(c.halted && jit.nativeInstructions == 1 && jit.compiledBlocks == 1,
              "immediate store did not execute natively");
    }
    for (bool immediateStore : {false, true}) {
        auto c = immediateStore
            ? make({0xc7, 0x03, 0xf4, 0x90, 0x90, 0x90, 0x90, 0x83, 0xc0, 1, 0xf4})
            : make({0x80, 0x0b, 0x64, 0x90, 0x83, 0xc0, 1, 0xf4});
        c.r[BX] = immediateStore ? 6 : 3;
        c.r[AX] = 42;
        compare(c); // Rewrite the next native instruction to HLT, skipping ADD AX.
    }
    for (int operation : {-2, -1, 0, 1, 2, 3, 4, 5, 6}) {
        auto c = operation < 0
            ? make({0xff, uint8_t(operation == -1 ? 3 : 11), 0xf4}, true)
            : make({0x81, uint8_t((operation << 3) | 3), 0, 1, 0, 0, 0xf4}, true);
        // The operand is its own PTE. The write-permission probe sets D in
        // that same word, but the ALU must use the value read before that probe.
        uint32_t selfPte = 0x81007;
        std::copy_n(reinterpret_cast<uint8_t *>(&selfPte), 4, c.mem.begin() + 0x81024);
        c.flushTLB();
        c.r[BX] = 0x9024;
        c.flags = 2 | CF;
        CPU ref = c;
        Jit jit(c, true, 0);
        jit.setDirectLinks(linksMode, 0);
        auto part = jit.runBatch();
        for (unsigned i = 0; i < part.instructions; ++i) ref.step();
        check(c.mem == ref.mem, "RMW observed its own write-probe dirty-bit update");
        equal(c, ref);
        check(c.nativeMemoryCalls && jit.nativeInstructions == 1,
              "self-PTE RMW did not exercise the native helper");
    }
    for (bool immediateStore : {false, true})
      for (bool watched : {false, true})
       for (unsigned operation : {0u, 2u, 3u}) {
        if (immediateStore && operation) continue;
        auto c = immediateStore ? make({0xc7, 0x03, 1, 2, 3, 4, 0xf4})
                                : make({0x83, uint8_t((operation << 3) | 3), 1, 0xf4});
        c.r[BX] = 0xa0000;
        CPU ref = c;
        std::vector<uint64_t> trace, refTrace;
        c.videoRead = [&](uint32_t p) { trace.push_back(uint64_t(p) << 8); return uint8_t(p); };
        ref.videoRead = [&](uint32_t p) { refTrace.push_back(uint64_t(p) << 8); return uint8_t(p); };
        c.videoWrite = [&](uint32_t p, uint8_t v) { trace.push_back((1ull << 60) | (uint64_t(p) << 8) | v); };
        ref.videoWrite = [&](uint32_t p, uint8_t v) { refTrace.push_back((1ull << 60) | (uint64_t(p) << 8) | v); };
        if (watched) {
            c.watchStart = ref.watchStart = 0xa0000;
            c.watchLength = ref.watchLength = 4;
            c.watchWrite = [&](uint32_t, uint32_t v, int) {
                trace.push_back((2ull << 60) | v); trace.push_back((3ull << 60) | c.flags);
            };
            ref.watchWrite = [&](uint32_t, uint32_t v, int) {
                refTrace.push_back((2ull << 60) | v); refTrace.push_back((3ull << 60) | ref.flags);
            };
        }
        Jit jit(c, true, 0);
        jit.setDirectLinks(linksMode, 0);
        while (!c.halted) {
            auto part = jit.runBatch();
            for (unsigned i = 0; i < part.instructions; ++i) ref.step();
            equal(c, ref);
        }
        check(trace == refTrace && trace.size() == (immediateStore ? 4u : 8u) + 2 * unsigned(watched),
              "native write replay changed MMIO/watch order or duplicated a callback");
        check(jit.compiledBlocks && c.nativeMemoryCalls, "write callback test did not exercise native replay");
    }
    for (bool immediateStore : {false, true})
      for (bool crossPage : {false, true})
       for (unsigned operation : {0u, 2u, 3u}) {
        if (immediateStore && operation) continue;
        auto c = immediateStore ? make({0x83, 0xc0, 1, 0xc7, 0x03, 1, 2, 3, 4, 0xf4}, true)
                                : make({0x83, 0xc0, 1, 0x83, uint8_t((operation << 3) | 3), 1, 0xf4}, true);
        c.s[CS].selector = 3;
        c.r[AX] = operation ? 0xffffffff : 0x7fffffff;
        c.r[BX] = crossPage ? 0x4fff : 0x5000;
        uint32_t readOnly = 0x5005;
        std::copy_n(reinterpret_cast<uint8_t *>(&readOnly), 4, c.mem.begin() + 0x81014);
        c.mem[0x4fff] = 0x7f;
        c.flushTLB();
        CPU ref = c;
        unsigned faults = 0;
        auto capture = [&](const Fault &f) {
            check(f.vector == 14 && f.error == 7, "native memory write fault type");
            ++faults;
            throw std::runtime_error("captured write fault");
        };
        c.observeFault = ref.observeFault = capture;
        Jit jit(c, true, 0);
        jit.setDirectLinks(linksMode, 0);
        try { jit.runBatch(); check(false, "native write fault missing"); }
        catch (const std::runtime_error &e) { check(std::string(e.what()) == "captured write fault", "native write fault observer"); }
        try { ref.step(); ref.step(); check(false, "reference write fault missing"); }
        catch (const std::runtime_error &e) { check(std::string(e.what()) == "captured write fault", "reference write fault observer"); }
        equal(c, ref);
        check(faults == 2 && c.ip == 3 && c.cr[2] == 0x5000 && c.mem[0x4fff] == 0x7f,
              "write fault lost prefix flags or partially modified memory");
    }
}

static void hotShortFragmentTests() {
    for (bool paging : {false, true})
        for (bool memory : {false, true}) {
            auto c = memory ? make({0x8b, 0x03, 0xf8, 0xf4}, paging)
                            : make({0x83, 0xc0, 1, 0xf8, 0xf4}, paging);
            c.r[BX] = 0x1000;
            c.write(0x1000, 0x12345678, 4);
            CPU ref = c;
            Jit jit(c, true, 8);
            jit.setDirectLinks(linksMode, 0);
            for (unsigned run = 0; run < 256; ++run) {
                if (run == 60) {
                    // Negative entries need no host-buffer guards, but their
                    // promotion must capture the current RAM allocation.
                    auto replacement = c.mem;
                    c.mem.swap(replacement);
                    std::fill(replacement.begin(), replacement.end(), 0xa5);
                    if (!memory) {
                        c.write(2, 2, 1);
                        ref.write(2, 2, 1);
                    }
                }
                c.ip = ref.ip = 0;
                c.halted = ref.halted = false;
                while (!c.halted) {
                    auto part = jit.runBatch();
                    check(part.instructions && part.instructions <= 127, "short fragment budget");
                    for (unsigned i = 0; i < part.instructions; ++i) ref.step();
                    equal(c, ref);
                }
                if (run == 15)
                    check(jit.nativeInstructions == 0, "cold short fragment compiled prematurely");
                if (!memory && run == 75)
                    check(jit.nativeInstructions == 0, "changed short code retained old promotion heat");
            }
            check(jit.compiledBlocks == 1 && jit.nativeInstructions > 100,
                  "hot supported short fragment was not promoted exactly once");
            if (paging && memory) {
                c.r[BX] = ref.r[BX] = 0x5000;
                std::fill_n(c.mem.begin() + 0x81014, 4, 0);
                std::fill_n(ref.mem.begin() + 0x81014, 4, 0);
                c.flushTLB(); ref.flushTLB();
                c.ip = ref.ip = 0;
                c.halted = ref.halted = false;
                unsigned faults = 0;
                auto capture = [&](const Fault &f) {
                    check(f.vector == 14 && f.error == 0, "promoted short memory fault type");
                    ++faults;
                    throw std::runtime_error("captured short fault");
                };
                c.observeFault = ref.observeFault = capture;
                auto calls = c.nativeMemoryCalls;
                try { jit.runBatch(); check(false, "promoted short fault missing"); }
                catch (const std::runtime_error &e) { check(std::string(e.what()) == "captured short fault", "short fault observer"); }
                try { ref.step(); check(false, "reference short fault missing"); }
                catch (const std::runtime_error &e) { check(std::string(e.what()) == "captured short fault", "reference short fault observer"); }
                equal(c, ref);
                check(faults == 2 && c.nativeMemoryCalls > calls && c.cr[2] == 0x5000,
                      "promoted short fragment did not replay its precise fault");
            }
        }
    {
        auto c = make({0xf3, 0xad, 0xf4});
        c.r[SI] = 0x1000;
        CPU ref = c;
        Jit jit(c, true, 8);
        jit.setDirectLinks(linksMode, 0);
        for (unsigned run = 0; run < 256; ++run) {
            c.ip = ref.ip = 0;
            c.r[CX] = ref.r[CX] = 2;
            c.halted = ref.halted = false;
            while (!c.halted) {
                auto part = jit.runBatch();
                for (unsigned i = 0; i < part.instructions; ++i) ref.step();
                equal(c, ref);
            }
        }
        check(jit.nativeInstructions == 0, "hot unsupported REP form was promoted");
    }
}

static void delayedPromotionTest() {
    auto c = make({0x83, 0xc0, 1, 0x83, 0xe9, 1, 0x75, 0xf8, 0xf4});
    c.r[CX] = 1000;
    CPU ref = c;
    Jit jit(c, true, 0, 65536);
    jit.setDirectLinks(true, 8);
    while (!c.halted) {
        auto part = jit.runBatch();
        check(part.instructions && part.instructions <= 127, "delayed promotion budget");
        for (unsigned i = 0; i < part.instructions; ++i) ref.step();
        equal(c, ref);
    }
    check(jit.directLinkPromotions == 1 && jit.directLinkBlocks == 1,
          "hot native entry did not promote exactly once");
    check(jit.compiledBlocks == 1, "promotion retranslates an existing guest body");
    check(jit.directLinkTransitions > 100, "hot native stub is unused");
}

static void replacedIncomingSourceTest() {
    auto c = make({0x8b, 0x03, 0x83, 0xc0, 1, 0xeb, 9});
    const uint8_t second[] = {0x83, 0xe9, 1, 0x75, 0xeb, 0xf4};
    std::copy(std::begin(second), std::end(second), c.mem.begin() + 16);
    c.r[BX] = 0x1000;
    c.write(0x1000, 123, 4);
    CPU ref = c;
    Jit jit(c, true, 0, 65536);
    jit.setDirectLinks(true, 0);
    for (unsigned replacement = 0; replacement < 500; ++replacement) {
        // Invalidate both ends while old incoming edges and memory-helper
        // descriptors exist. Repeated replacement also forces arena reuse.
        c.write(4, 1 + (replacement & 1), 1);
        ref.write(4, 1 + (replacement & 1), 1);
        c.ip = ref.ip = 0;
        c.halted = ref.halted = false;
        c.r[CX] = ref.r[CX] = 50;
        while (!c.halted) {
            auto part = jit.runBatch();
            check(part.instructions && part.instructions <= 127, "replacement budget");
            for (unsigned i = 0; i < part.instructions; ++i) ref.step();
            equal(c, ref);
        }
    }
    check(jit.directLinkTransitions > 1000, "replacement test did not exercise links");
    check(jit.cacheRecycles > 0, "replacement test did not recycle native addresses");
}

int main() {
    try {
        CPU probe(1024 * 1024);
        for (size_t bytes : {size_t(65537), size_t(128 * 1024 * 1024)}) {
            Jit unsupported(probe, false, 8, bytes);
            bool rejected = false;
            try { unsupported.setDirectLinks(true); }
            catch (const std::invalid_argument &) { rejected = true; }
            check(rejected, "unsafe direct-link arena accepted");
        }
        bool overflowRejected = false;
        try { Jit overflow(probe, false, 8, ~size_t(0)); }
        catch (const std::invalid_argument &) { overflowRejected = true; }
        check(overflowRejected, "overflowing arena allocation accepted");
        Jit available(probe);
        if (!available.enabled()) {
            std::cout << "Skipped: native JIT unavailable\n";
            return 0;
        }
        for (bool enabled : {false, true}) {
            linksMode = enabled;
            memoryTests();
            stringMoveTests();
            memoryWriteCoverageTests();
            memoryExchangeTests();
            memoryBitStringTests();
            hotShortFragmentTests();
            cycleTests();
        }
        check(observedLinks > 0, "direct links were never exercised");
        linkedContextTests();
        delayedPromotionTest();
        replacedIncomingSourceTest();
        debugMapTests();
        std::cout << "Passed " << checks
                  << " cached batch checks; multi-dispatch batches=" << multiDispatchBatches
                  << "; linked transitions=" << observedLinks << "\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
