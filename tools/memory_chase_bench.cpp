// Diskless x86 dependent-load benchmark. Initialization and verification are untimed.
#include "cpu.hpp"
#include "jit.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <numeric>
#include <random>
#include <string>
using namespace pc;

static void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}

int main(int argc, char **argv) {
    try {
        unsigned loops = 10000000;
        std::string backend = "jit", dispatch = "batch", paging = "both";
        for (int arg = 1; arg < argc; arg += 2) {
            require(arg + 1 < argc, "Options require a value");
            std::string option = argv[arg], value = argv[arg + 1];
            if (option == "--loops") {
                size_t end = 0;
                auto count = std::stoull(value, &end);
                require(end == value.size() && count && count <= 100000000,
                        "--loops must be 1..100000000");
                loops = unsigned(count);
            } else if (option == "--backend")
                backend = value;
            else if (option == "--dispatch")
                dispatch = value;
            else if (option == "--paging")
                paging = value;
            else
                throw std::runtime_error("Unknown option: " + option);
        }
        require(backend == "jit" || backend == "interpreter", "Invalid backend");
        require(dispatch == "single" || dispatch == "batch", "Invalid dispatch mode");
        require(paging == "flat" || paging == "paged" || paging == "both", "Invalid paging mode");
        std::cout.precision(17);
        std::cout
            << "{\"scope\":\"CPU/JIT-only dependent RAM loads; cold JIT; no devices or disk\","
               "\"backend\":\""
            << backend << "\",\"dispatch\":\"" << dispatch << "\",\"loops\":" << loops
            << ",\"seed\":12648430,\"samples\":[";
        bool first = true;
        for (bool paged : {false, true}) {
            if ((paged && paging == "flat") || (!paged && paging == "paged"))
                continue;
            for (unsigned bytes : {4096u, 32768u, 524288u, 8388608u}) {
                constexpr unsigned base = 0x200000, stride = 64;
                CPU cpu(16 * 1024 * 1024);
                cpu.cr[0] = 0x11;
                cpu.s[CS].big = true;
                for (auto &segment : cpu.s)
                    segment.limit = 0xffffffffu;
                // MOV EBX,[EBX]; ADD EAX,[EBX+4]; SUB ECX,1; JNZ 0; HLT.
                const uint8_t code[] = {0x8b, 0x1b, 0x03, 0x43, 4, 0x83, 0xe9, 1, 0x75, 0xf6, 0xf4};
                std::copy(std::begin(code), std::end(code), cpu.mem.begin());
                std::vector<unsigned> order(bytes / stride);
                std::iota(order.begin(), order.end(), 0);
                std::mt19937 random(0xc0ffee);
                std::shuffle(order.begin(), order.end(), random);
                for (size_t i = 0; i < order.size(); ++i) {
                    cpu.write(base + order[i] * stride,
                              base + order[(i + 1) % order.size()] * stride, 4);
                    cpu.write(base + order[i] * stride + 4, 1, 4);
                }
                if (paged) {
                    constexpr unsigned directory = 0xe00000;
                    for (unsigned i = 0; i < 4; ++i) {
                        unsigned table = directory + 4096 * (i + 1);
                        cpu.write(directory + i * 4, table | 7, 4);
                        for (unsigned j = 0; j < 1024; ++j)
                            cpu.write(table + j * 4, ((i * 1024 + j) << 12) | 7, 4);
                    }
                    cpu.cr[3] = directory;
                    cpu.cr[0] |= 0x80000000;
                    cpu.flushTLB();
                }
                cpu.r[BX] = base + order[0] * stride;
                cpu.r[CX] = loops;
                cpu.observeFault = [](const Fault &) { throw std::runtime_error("Guest fault"); };
                Jit jit(cpu, backend == "jit", 8, 16 * 1024 * 1024, Jit::BlockMode::FollowJumps);
                require(backend != "jit" || jit.enabled(), "Native JIT unavailable");
                const uint64_t expected = uint64_t(loops) * 4 + 1;
                auto start = std::chrono::steady_clock::now();
                while (!cpu.halted && cpu.instructions < expected) {
                    if (dispatch == "batch")
                        jit.runBatch();
                    else
                        jit.run();
                }
                auto seconds =
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                require(cpu.halted && cpu.ip == sizeof(code) && cpu.instructions == expected &&
                            cpu.r[CX] == 0 && cpu.r[AX] == loops &&
                            cpu.r[BX] == base + order[loops % order.size()] * stride,
                        "Incorrect guest result");
                for (size_t i = 0; i < order.size(); ++i)
                    require(cpu.read(base + order[i] * stride, 4) ==
                                    base + order[(i + 1) % order.size()] * stride &&
                                cpu.read(base + order[i] * stride + 4, 4) == 1,
                            "Guest changed the read-only ring");
                std::cout << (first ? "" : ",") << "{\"bytes\":" << bytes
                          << ",\"paged\":" << (paged ? "true" : "false")
                          << ",\"hostSeconds\":" << seconds
                          << ",\"guestInstructions\":" << cpu.instructions
                          << ",\"nativeInstructions\":" << jit.nativeInstructions
                          << ",\"nativeCalls\":" << jit.cacheHits
                          << ",\"slowMemoryCalls\":" << cpu.nativeMemoryCalls
                          << ",\"checksum\":" << cpu.r[AX] << "}";
                first = false;
            }
        }
        std::cout << "]}\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
