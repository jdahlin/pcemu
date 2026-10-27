#include "cpu.hpp"
#include "profile.hpp"
#include <iostream>
#include <stdexcept>
using namespace pc;
static uint64_t ticks, tickStep;
static unsigned checks;
static uint64_t fakeClock() noexcept { auto now = ticks; ticks += tickStep; return now; }
static uint64_t unavailableClock() noexcept { return UINT64_MAX; }
static void check(bool value, const char *message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
static void begin() {
    ticks = tickStep = 0;
    profile::instructionClockNs = fakeClock;
    profile::start(0);
}
int main() {
    try {
        begin();
        tickStep = 5;
        profile::start(10);
        check(profile::data.clockPairMeanNs == 5, "clock baseline");
        {
            profile::InstructionScope scope;
            profile::opcode(0x0f, 0, 0, 0, 0);
            profile::extended(0xa3);
            ticks += 100;
        }
        auto &bt = profile::data.cpuTiming[0x1a3];
        check(bt.attempts == 1 && bt.samples == 1 && bt.rawNs == 105 && profile::adjustedCpuNs(bt) == 100,
              "extended opcode CPU attribution");
        check(profile::data.cpuTiming[15].attempts == 0 && profile::data.opcodes[15] == 1,
              "extended timing double counted primary 0F");
        check(profile::data.attempts == 1 && profile::data.sampledIPs.size() == 1, "original accounting changed");
        begin();
        profile::data.clockPairMeanNs = 10;
        for (unsigned elapsed : {5u, 25u}) {
            profile::data.untilCpuSample = 1;
            profile::InstructionScope scope;
            profile::opcode(0x90, 0, 0, 0, 0);
            ticks += elapsed;
        }
        check(profile::data.cpuTiming[0x90].rawNs == 30 && profile::adjustedCpuNs(profile::data.cpuTiming[0x90]) == 10,
              "clock correction clipped individual samples");
        begin();
        {
            profile::InstructionScope outer;
            profile::opcode(0x90, 0, 0, 0, 0); ticks += 10;
            {
                profile::InstructionScope inner;
                profile::opcode(0x91, 0, 0, 0, 0); ticks += 50;
            }
            ticks += 30;
        }
        check(profile::data.cpuTiming[0x90].rawNs == 90 && profile::data.cpuTiming[0x91].samples == 0 &&
              profile::data.nestedScopes == 1 && !profile::currentScope, "nested scope accounting");
        begin();
        {
            profile::InstructionScope old;
            profile::opcode(0x90, 0, 0, 0, 0);
            profile::start(0);
            { profile::InstructionScope fresh; profile::opcode(0x91, 0, 0, 0, 0); ticks += 77; }
        }
        check(profile::data.cpuTiming[0x90].samples == 0 && profile::data.cpuTiming[0x91].rawNs == 77 &&
              !profile::currentScope, "interval reset attributed old scope");
        begin();
        { profile::InstructionScope scope; profile::opcode(0x90, 0, 0, 0, 0); profile::active = false; ticks += 50; }
        check(profile::data.cpuTiming[0x90].samples == 0, "stopped interval included pending scope");
        begin();
        { profile::InstructionScope scope; ticks += 31; }
        check(profile::data.unknownTiming.samples == 1 && profile::data.unknownTiming.rawNs == 31,
              "unknown/fetch scope lost");
        begin();
        profile::instructionClockNs = unavailableClock;
        { profile::InstructionScope scope; profile::opcode(0x90, 0, 0, 0, 0); }
        check(profile::data.clockFailures == 1 && profile::data.cpuTiming[0x90].samples == 0,
              "unavailable clock accepted");
        begin(); ticks = 100;
        { profile::InstructionScope scope; profile::opcode(0x90, 0, 0, 0, 0); ticks = 90; }
        check(profile::data.clockFailures == 1 && profile::data.cpuTiming[0x90].samples == 0,
              "backwards clock accepted");
        begin();
        {
            CPU c(65536); c.cr[0] = 0x11; c.s[CS].big = true;
            c.mem[0] = 0xc7; c.mem[1] = 3; c.mem[2] = 1; c.r[BX] = 0x1000;
            c.watchStart = 0x1000; c.watchLength = 4;
            c.watchWrite = [](uint32_t, uint32_t, int) { ticks += 101; };
            c.step();
            check(profile::data.cpuTiming[0xc7].rawNs == 101 && profile::data.cpuTiming[0xc7].attempts == 1,
                  "CPU::step does not bracket execution");
        }
        begin();
        {
            CPU c(65536); c.cr[0] = 0x11; c.s[CS].big = true; c.s[FS].unusable = true;
            c.mem[0] = 0x64; c.mem[1] = 0x8b; c.mem[2] = 3;
            c.observeFault = [](const Fault &) { ticks += 203; throw std::runtime_error("expected fault"); };
            try { c.step(); check(false, "missing observed fault"); }
            catch (const std::runtime_error &e) { check(std::string(e.what()) == "expected fault", "unexpected fault error"); }
            const auto &time = profile::data.cpuTiming[0x8b];
            check(time.rawNs == 203 && time.faults == 1 && time.exceptions == 1 && c.ip == 0,
                  "fault timing/error attribution changed restart");
        }
        profile::instructionClockNs = profile::threadCpuNs;
        profile::start(0);
        const auto first = profile::threadCpuNs(), second = profile::threadCpuNs();
#if defined(__APPLE__) || defined(__linux__)
        check(first != UINT64_MAX && second >= first && profile::data.cpuClockAvailable, "real thread CPU clock unavailable");
#else
        check(first == UINT64_MAX && second == UINT64_MAX && !profile::data.cpuClockAvailable, "unsupported clock mislabeled");
#endif
        profile::active = false;
        std::cout << "Passed " << checks << " guest profile timing checks\n";
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
