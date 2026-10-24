#include "jit.hpp"
#include "machine.hpp"
#include <algorithm>
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
    bool cold = false, hasChecksum = false;
    uint32_t checksum = 0;
};
static uint32_t expectedChecksum(unsigned id, unsigned loops) {
    switch (id) {
    case 16:
        return (loops * 48u) & 65535;
    case 17:
        return (loops * 8u) & 65535;
    case 18:
        return (loops * 2u + (loops & 1)) & 65535;
    case 19:
        return (loops * 4u) & 65535;
    case 20:
        return 64u * 0x5a5a;
    case 21:
        return (loops * 3u) & 65535;
    case 32:
        return loops * 48u;
    case 33:
        return loops * 8u;
    case 34:
        return loops * 2u + (loops & 1);
    case 35:
        return loops * 4u;
    case 36:
        return uint32_t(64ull * 0x5a5a5a5a);
    case 37:
        return loops * 3u;
    case 22:
    case 38:
        return ((loops & 255) << 8) | ((loops >> 8) & 255);
    default:
        throw std::runtime_error("Unknown microbenchmark case");
    }
}
int main(int argc, char **argv) {
    try {
        std::string disk, output;
        bool native = true;
        unsigned timeout = 180, loops = 1000000;
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
            else if (arg == "--loops")
                loops = std::stoul(value());
            else if (arg == "--no-jit")
                native = false;
            else
                throw std::runtime_error("Unknown option: " + arg);
        }
        if (disk.empty() || output.empty() || !timeout || !loops || loops > 10000000)
            throw std::runtime_error("Usage: guest-bench --disk PRIVATE.pcd --output RESULT.json "
                                     "[--no-jit] [--timeout SECONDS]");
        Machine m(disk, "", 16 * 1024 * 1024);
        auto &c = m.cpu;
        Jit jit(c, native);
        const auto processStart = Clock::now();
        Clock::time_point start;
        std::vector<Sample> samples;
        unsigned id = 0;
        bool measuring = false, done = false, clear = false, fresh = false, sampleCold = false;
        uint64_t ins = 0, nat = 0, mem = 0, blocks = 0, recycles = 0;
        std::string transcript;
        c.observeFault = [&](const Fault &f) {
            std::ostringstream s;
            s << "Guest fault " << unsigned(f.vector) << " error=" << f.error << " at "
              << c.location() << " CR2=" << std::hex << c.cr[2];
            throw std::runtime_error(s.str());
        };
        c.input = [&](uint16_t port, int width) {
            if (port == 0xe4 && width == 4)
                return loops;
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
            if (port == 0xe5 && width == 4) {
                if (measuring || samples.empty() || samples.back().id != id ||
                    samples.back().hasChecksum)
                    throw std::runtime_error("Checksum without a completed interval");
                if (value != expectedChecksum(id, loops)) {
                    std::ostringstream error;
                    error << "Case " << id << " checksum " << value << " expected "
                          << expectedChecksum(id, loops);
                    throw std::runtime_error(error.str());
                }
                samples.back().checksum = value;
                samples.back().hasChecksum = true;
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
                    if (!samples.empty() && samples.back().id != 1 && !samples.back().hasChecksum)
                        throw std::runtime_error("Previous interval has no checksum");
                    sampleCold = fresh;
                    fresh = false;
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
                                       jit.cacheRecycles - recycles, sampleCold});
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
                fresh = true;
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
        const bool coremark =
            std::any_of(samples.begin(), samples.end(), [](const Sample &s) { return s.id == 1; });
        const bool micro =
            std::any_of(samples.begin(), samples.end(), [](const Sample &s) { return s.id != 1; });
        if (micro) {
            for (unsigned id : {16u, 17u, 18u, 19u, 20u, 21u, 22u, 32u, 33u, 34u, 35u, 36u, 37u, 38u}) {
                unsigned count = 0;
                uint64_t firstInstructions = 0;
                for (const auto &s : samples)
                    if (s.id == id) {
                        if (!s.hasChecksum || s.cold != (count == 0))
                            throw std::runtime_error("Missing micro checksum or cold/warm state");
                        if (!count)
                            firstInstructions = s.instructions;
                        else if (s.instructions != firstInstructions)
                            throw std::runtime_error("Cold/warm guest instruction counts differ");
                        ++count;
                    }
                if (count != 2)
                    throw std::runtime_error("Microbenchmark matrix incomplete");
            }
        }
        if (coremark && (!crc || transcript.find("]ERROR!") != std::string::npos ||
                         transcript.find("ERROR:") != std::string::npos ||
                         transcript.find("Cannot validate") != std::string::npos))
            throw std::runtime_error("CoreMark CRC validation failed: " + transcript);
        std::ofstream text(output + ".guest.txt");
        text << transcript;
        std::ofstream result(output);
        if (!result)
            throw std::runtime_error("Cannot write results");
        result << "{\"scope\":\"actual minimal DOS boot, original protected-mode loader, host "
                  "monotonic wall time, no SDL\",\"jit\":"
               << (native ? "true" : "false") << ",\"returnedToDos\":true,\"crcValidated\":"
               << (coremark && crc ? "true" : "false")
               << ",\"microValidated\":" << (micro ? "true" : "false") << ",\"loops\":" << loops
               << ",\"upstreamValidated\":"
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
                   << ",\"jitClearedBefore\":" << (s.cold ? "true" : "false")
                   << ",\"checksumChecked\":" << (s.hasChecksum ? "true" : "false")
                   << ",\"checksum\":" << s.checksum << '}';
        }
        result << "]}\n";
        m.disk->flush();
        std::cout << "PASS: Guest results validated, " << samples.size()
                  << " measured interval(s), returned to real-mode DOS; last="
                  << samples.back().seconds << " s\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
