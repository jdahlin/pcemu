// CPU/JIT-only RAM harness for the same COM that the QEMU BIOS loader runs.
#include "cpu.hpp"
#include "jit.hpp"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>
using namespace pc;
using Clock = std::chrono::steady_clock;
int main(int argc, char **argv) {
    try {
        if (argc != 5 || std::string(argv[3]) != "--backend")
            throw std::runtime_error("Usage: memory-bench PROGRAM.COM OUTPUT.json --backend jit|interpreter");
        const std::string backend = argv[4];
        if (backend != "jit" && backend != "interpreter") throw std::runtime_error("Unknown backend");
        std::ifstream input(argv[1], std::ios::binary);
        if (!input) throw std::runtime_error("Cannot read program");
        std::vector<uint8_t> program{std::istreambuf_iterator<char>(input), {}};
        if (program.empty() || program.size() > 65000) throw std::runtime_error("Invalid COM size");
        CPU cpu(64 * 1024 * 1024);
        for (unsigned seg : {CS, DS, ES, SS}) {
            cpu.s[seg].base = 0x10000; cpu.s[seg].selector = 0x1000;
        }
        cpu.ip = 0x100; cpu.r[SP] = 0xfffe;
        std::copy(program.begin(), program.end(), cpu.mem.begin() + 0x10100);
        Jit jit(cpu, backend == "jit", 8, 16 * 1024 * 1024, Jit::BlockMode::FollowJumps);
        if (backend == "jit" && !jit.enabled()) throw std::runtime_error("Native JIT unavailable");
        cpu.observeFault = [&](const Fault &f) {
            throw std::runtime_error("Guest fault " + std::to_string(f.vector) + " at " + cpu.location());
        };
        cpu.input = [](uint16_t p, int) { return p == 0x92 ? 2u : 0xffu; };
        bool done = false, measuring = false;
        std::string transcript;
        struct Sample { double seconds; uint64_t instructions, native, helpers; };
        std::vector<Sample> samples;
        Clock::time_point start;
        uint64_t instructions = 0, native = 0, helpers = 0;
        cpu.output = [&](uint16_t port, uint32_t value, int width) {
            if (port == 0xe9 && width == 1) transcript.push_back(char(value));
            if (port != 0xe7 || width != 1) return;
            if (value == 1 && !measuring && !done) {
                instructions = cpu.instructions; native = jit.nativeInstructions;
                helpers = cpu.nativeMemoryCalls; measuring = true; start = Clock::now();
            } else if (value == 2 && measuring) {
                auto seconds = std::chrono::duration<double>(Clock::now() - start).count();
                samples.push_back({seconds, cpu.instructions - instructions,
                    jit.nativeInstructions - native, cpu.nativeMemoryCalls - helpers});
                measuring = false;
            } else if (value == 3 && !measuring && !samples.empty()) done = true;
            else throw std::runtime_error("Invalid interval marker");
        };
        const auto deadline = Clock::now() + std::chrono::minutes(5);
        unsigned dispatches = 0;
        while (!done) {
            jit.run();
            if ((++dispatches & 1023) == 0 && Clock::now() >= deadline) throw std::runtime_error("Guest timeout");
        }
        std::ofstream text(std::string(argv[2]) + ".guest.txt"); text << transcript;
        std::ofstream output(argv[2]);
        if (!output || !text) throw std::runtime_error("Cannot write results");
        output.precision(17);
        output << "{\"backend\":\"" << backend << "\",\"scope\":\"CPU/JIT-only RAM execution, no Machine/device polling\",\"samples\":[";
        for (size_t n = 0; n < samples.size(); ++n) {
            const auto &s = samples[n];
            output << (n ? "," : "") << "{\"hostSeconds\":" << s.seconds
                   << ",\"guestInstructions\":" << s.instructions << ",\"nativeInstructions\":" << s.native
                   << ",\"slowMemoryCalls\":" << s.helpers << "}";
        }
        output << "]}\n";
        std::cout << samples.size() << " intervals completed\n";
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
