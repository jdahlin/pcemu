#include "jit.hpp"
#include "machine.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
using namespace pc;
using Clock = std::chrono::steady_clock;
struct Sample {
    unsigned id;
    double seconds;
    uint64_t instructions, native, memory, blocks, recycles;
};
int main(int argc, char **argv) {
    try {
        std::string disk, output;
        bool native = true;
        unsigned timeout = 180;
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            auto value = [&] {
                if (++i >= argc)
                    throw std::runtime_error("Missing option value");
                return std::string(argv[i]);
            };
            if (arg == "--disk")
                disk = value();
            else if (arg == "--output")
                output = value();
            else if (arg == "--timeout")
                timeout = std::stoul(value());
            else if (arg == "--no-jit")
                native = false;
            else
                throw std::runtime_error("Unknown option: " + arg);
        }
        if (disk.empty() || output.empty() || !timeout)
            throw std::runtime_error("Usage: guest-bench --disk PRIVATE.pcd --output RESULT.json "
                                     "[--no-jit] [--timeout SECONDS]");
        Machine m(disk, "", 16 * 1024 * 1024);
        auto &c = m.cpu;
        Jit jit(c, native);
        const auto processStart = Clock::now();
        Clock::time_point start;
        std::vector<Sample> samples;
        unsigned id = 0;
        bool measuring = false, done = false, clear = false;
        uint64_t ins = 0, nat = 0, mem = 0, blocks = 0, recycles = 0;
        std::string transcript;
        c.observeFault = [&](const Fault &f) {
            std::ostringstream s;
            s << "Guest fault " << unsigned(f.vector) << " error=" << f.error << " at "
              << c.location() << " CR2=" << std::hex << c.cr[2];
            throw std::runtime_error(s.str());
        };
        c.input = [&](uint16_t port, int width) {
            if (port == 0xe8 && width == 4)
                return uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() -
                                                                                      processStart)
                                    .count());
            return m.in(port, width);
        };
        c.output = [&](uint16_t port, uint32_t value, int width) {
            if (port == 0xe9 && width == 1) {
                transcript.push_back(char(value));
                return;
            }
            if (port == 0xe6 && width == 1) {
                if (measuring)
                    throw std::runtime_error("Case ID changed inside measurement");
                id = value;
                return;
            }
            if (port == 0xe7 && width == 1) {
                if (value == 1) {
                    if (measuring || !id)
                        throw std::runtime_error("Invalid BEGIN");
                    ins = c.instructions;
                    nat = jit.nativeInstructions;
                    mem = c.nativeMemoryCalls;
                    blocks = jit.compiledBlocks;
                    recycles = jit.cacheRecycles;
                    measuring = true;
                    start = Clock::now();
                } else if (value == 2) {
                    auto end = Clock::now();
                    if (!measuring)
                        throw std::runtime_error("END without BEGIN");
                    samples.push_back({id, std::chrono::duration<double>(end - start).count(),
                                       c.instructions - ins, jit.nativeInstructions - nat,
                                       c.nativeMemoryCalls - mem, jit.compiledBlocks - blocks,
                                       jit.cacheRecycles - recycles});
                    measuring = false;
                } else if (value == 3) {
                    if (measuring)
                        throw std::runtime_error("DONE inside measurement");
                    done = true;
                } else if (value == 4) {
                    if (measuring)
                        throw std::runtime_error("JIT clear inside measurement");
                    clear = true;
                } else
                    throw std::runtime_error("Unknown measurement command");
                return;
            }
            m.out(port, value, width);
        };
        m.boot();
        uint64_t nextCheck = 0;
        bool returned = false;
        while (true) {
            if (!c.halted)
                jit.run();
            else if (c.flags & IF)
                m.timer();
            if (clear) {
                jit.clear();
                clear = false;
            }
            m.poll();
            if (c.instructions >= nextCheck || c.halted) {
                nextCheck = c.instructions + 100000;
                if (done && m.text().find("PCEMU benchmark returned to DOS") != std::string::npos) {
                    returned = true;
                    break;
                }
                if (Clock::now() - processStart > std::chrono::seconds(timeout))
                    throw std::runtime_error("Guest benchmark timed out at " + c.location());
            }
        }
        if ((c.cr[0] & 1) || c.s[CS].big)
            throw std::runtime_error("Benchmark did not restore real-mode DOS");
        if (!returned || samples.empty() || measuring)
            throw std::runtime_error("Benchmark did not complete");
        const bool crc = transcript.find("crclist       : 0xe714") != std::string::npos &&
                         transcript.find("crcmatrix     : 0x1fd7") != std::string::npos &&
                         transcript.find("crcstate      : 0x8e3a") != std::string::npos;
        if (!crc || transcript.find("]ERROR!") != std::string::npos)
            throw std::runtime_error("CoreMark CRC validation failed: " + transcript);
        std::ofstream text(output + ".guest.txt");
        text << transcript;
        std::ofstream result(output);
        if (!result)
            throw std::runtime_error("Cannot write results");
        result << "{\"scope\":\"actual minimal DOS boot, original protected-mode loader, host "
                  "monotonic wall time, no SDL\",\"jit\":"
               << (native ? "true" : "false")
               << ",\"returnedToDos\":true,\"crcValidated\":true,\"upstreamValidated\":"
               << (transcript.find("Correct operation validated.") != std::string::npos ? "true"
                                                                                        : "false")
               << ",\"samples\":[";
        for (size_t i = 0; i < samples.size(); ++i) {
            const auto &s = samples[i];
            if (i)
                result << ',';
            result << std::setprecision(12) << "{\"case\":" << s.id
                   << ",\"hostSeconds\":" << s.seconds
                   << ",\"guestInstructions\":" << s.instructions
                   << ",\"nativeInstructions\":" << s.native << ",\"slowMemoryCalls\":" << s.memory
                   << ",\"compiledBlocks\":" << s.blocks << ",\"cacheRecycles\":" << s.recycles
                   << '}';
        }
        result << "]}\n";
        m.disk->flush();
        std::cout << "PASS: CoreMark CRCs, " << samples.size()
                  << " measured interval(s), returned to real-mode DOS; last="
                  << samples.back().seconds << " s\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
