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
void compare(CPU c, unsigned batches = 10000) {
    CPU ref = c, single = c;
    Jit jit(c, true, 0, 65536, Jit::BlockMode::FollowJumps),
        one(single, true, 0, 65536, Jit::BlockMode::FollowJumps);
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
