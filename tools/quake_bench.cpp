// Headless runner for the unmodified DOS Quake timedemo. A private DOS disk
// starts -condebug +timedemo; observing DOS file writes supplies console output
// without OCR, guest RAM scanning, executable patches or concurrent disk readers.
#include "machine.hpp"
#include "jit.hpp"
#include "quake_benchmark.hpp"
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <thread>
#include <unordered_set>
using namespace pc;
using Clock = std::chrono::steady_clock;
int main(int argc, char **argv) {
    try {
        std::string disk, output;
        unsigned timeout = 1800;
        for (int n = 1; n < argc; ++n) {
            std::string arg = argv[n];
            if (n + 1 >= argc) throw std::runtime_error("Missing argument for " + arg);
            if (arg == "--disk") disk = argv[++n];
            else if (arg == "--output") output = argv[++n];
            else if (arg == "--timeout") timeout = std::stoul(argv[++n]);
            else throw std::runtime_error("Unknown argument: " + arg);
        }
        if (disk.empty() || output.empty() || !timeout)
            throw std::runtime_error("Usage: quake-bench --disk PRIVATE.qcow2 --output RESULT.json [--timeout SECONDS]");
        Machine machine(disk, "", 32 * 1024 * 1024);
        auto &cpu = machine.cpu;
        // 8254 CLK is external board wiring. Only this runner supplies wall time;
        // the emulator's normal instruction-clock and snapshot behavior remain.
        // Tick conversion avoids multiplying a long-running nanosecond counter.
        auto origin = Clock::now();
        machine.pitClock = [&] {
            auto ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - origin).count());
            return (ns / 1000000000) * 1193182 + ((ns % 1000000000) * 1193182) / 1000000000;
        };
        machine.pit.reset(0);
        Jit jit(cpu, false); // The interpreter is the verified original Quake backend.
        std::ofstream console(output + ".console.log");
        if (!console) throw std::runtime_error("Cannot write console transcript");
        std::string transcript;
        std::optional<QuakeTimedemoResult> result;
        bool resultClosing = false;
        unsigned resultHandle = 0;
        Clock::time_point resultTime;
        std::unordered_set<uint32_t> dosEntries;
        // CWSDPMI may call the saved real-mode DOS vector with PUSHF/CALL FAR,
        // bypassing CPU::bios(INT 21h). Observe handler entries, remembering
        // vectors installed during boot, rather than intercepting only INT.
        auto observeDosWrite = [&] {
            if ((cpu.cr[0] & 1) && !(cpu.flags & 0x20000)) return;
            const auto &mem = cpu.mem;
            auto word = [&](unsigned at) { return unsigned(mem[at]) | (unsigned(mem[at + 1]) << 8); };
            dosEntries.insert((word(0x86) << 4) + word(0x84));
            if (!dosEntries.contains(cpu.s[CS].base + cpu.ip)) return;
            // Con_DebugLog opens, writes and closes the log for each print.
            // Wait for this matching close to return to the 32-bit client before
            // stopping emulation; observing a write request alone is not success.
            if (result && cpu.get(4, 1) == 0x3e && cpu.get(BX, 2) == resultHandle)
                resultClosing = true;
            if (cpu.get(4, 1) != 0x40) return;
            auto length = cpu.get(CX, 2), at = cpu.address(DS, cpu.get(DX, 2));
            if (length && length <= 16384) {
                std::string write;
                write.reserve(length);
                for (unsigned n = 0; n < length; ++n) write += char(cpu.read(at + n, 1));
                console << write << std::flush;
                transcript += write;
                if (!result) {
                    result = parseQuakeTimedemo(transcript);
                    if (result) { resultHandle = cpu.get(BX, 2); resultTime = Clock::now(); }
                }
                if (transcript.size() > 262144) transcript.erase(0, transcript.size() - 131072);
            }
        };
        machine.boot();
        uint64_t nextCheck = 0;
        bool completed = false;
        while (true) {
            observeDosWrite();
            if (!cpu.halted) jit.run();
            machine.poll();
            if (cpu.instructions < nextCheck && !cpu.halted) continue;
            nextCheck = cpu.instructions + 20000;
            if (Clock::now() - origin > std::chrono::seconds(timeout))
                throw std::runtime_error("Quake timedemo timed out at " + cpu.location());
            if (result && Clock::now() - resultTime > std::chrono::seconds(30))
                throw std::runtime_error("Timedemo log close did not return at " + cpu.location());
            if (resultClosing && (cpu.cr[0] & 1) && cpu.cpl() == 3 && cpu.s[CS].big) {
                completed = true;
                break;
            }
            if (!(cpu.cr[0] & 1) && machine.text().find("PCEMU QUAKE BENCHMARK DONE") != std::string::npos)
                throw std::runtime_error("Quake returned to DOS without completing a timedemo");
            if (cpu.halted) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!result || !completed) throw std::runtime_error("Quake exited without a completed timedemo result");
        machine.disk->flush();
        std::ofstream report(output);
        if (!report) throw std::runtime_error("Cannot write benchmark result");
        report << std::setprecision(12) << "{\"frames\":" << result->frames << ",\"seconds\":" << result->seconds
               << ",\"fps\":" << result->fps << ",\"backend\":\"interpreter\",\"pitClock\":\"host-monotonic-1193182Hz\","
               << "\"resultFileClosed\":true,\"instructions\":" << cpu.instructions << ",\"totalWallSeconds\":"
               << std::chrono::duration<double>(Clock::now() - origin).count() << "}\n";
        std::cout << result->frames << " frames, " << result->seconds << " seconds: " << result->fps << " FPS\n";
    } catch (const std::exception &error) {
        std::cerr << "quake-bench: " << error.what() << '\n';
        return 1;
    }
}
