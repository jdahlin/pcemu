#include "cpu.hpp"
#include "jit.hpp"
#include "profile.hpp"
#include <iostream>
#include <cstring>
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
        profile::x87(0xd9, 0xe8); profile::x87(0xd9, 0xe8); profile::x87(0xdb, 0x1d);
        check(profile::data.x87[1][0xe8] == 2 && profile::data.x87[3][0x1d] == 1,
              "x87 profile distinguishes exact opcode/ModR/M forms");
        profile::active = false; profile::x87(0xd9, 0xe8);
        check(profile::data.x87[1][0xe8] == 2, "inactive x87 profile does not count");
        begin();
        check(profile::data.x87[1][0xe8] == 0, "new interval resets x87 diagnostics");
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
        begin();
        {
            profile::InterpretationScope warming(profile::InterpretationReason::Warming);
            profile::opcode(0x0f,0,0,0,0); profile::extended(0xa3);
            try {
                profile::InterpretationScope replay(profile::InterpretationReason::Replay);
                profile::opcode(0x8b,0,0,0,0);
                throw 1;
            } catch(int) {}
            profile::opcode(0x8b,0,0,0,0);
        }
        profile::opcode(0x8b,0,0,0,0);
        check(profile::data.interpretationReasons[0xf][unsigned(profile::InterpretationReason::Warming)]==1 &&
              profile::data.interpretationReasons[0x1a3][unsigned(profile::InterpretationReason::Warming)]==1,
              "extended dispatch reason attribution");
        for(auto reason:{profile::InterpretationReason::Warming,profile::InterpretationReason::Replay,profile::InterpretationReason::OutsideJit})
            check(profile::data.interpretationReasons[0x8b][unsigned(reason)]==1,"nested reason restored after exception");
        for(unsigned mode=0;mode<4;++mode) {
#if !defined(__APPLE__) || !defined(__aarch64__)
            if(mode>=2)continue;
#endif
            begin(); CPU c(65536);c.s[CS].big=true;
            const uint8_t code[]={0x40,0x43,0x41,0xf4};
            std::copy(std::begin(code),std::end(code),c.mem.begin());
            if(mode==2)c.mem[0]=0xf4; // Unsupported first instruction.
            if(mode==3)c.mem[1]=0xf4; // Supported single-instruction fragment.
            Jit jit(c,mode!=0,mode==1?8:1,65536);jit.run();
            const auto reason=mode==0?profile::InterpretationReason::Forced:mode==1?profile::InterpretationReason::Warming:
                              mode==2?profile::InterpretationReason::Declined:profile::InterpretationReason::ShortFragment;
            check(profile::data.interpretationReasons[c.mem[0]][unsigned(reason)]==1,"actual JIT dispatch reason");
            check(profile::interpretationReason==profile::InterpretationReason::OutsideJit,"JIT reason leaked to caller");
        }
#if defined(__APPLE__) && defined(__aarch64__)
        {
            begin(); CPU c(65536);c.s[CS].big=true;c.r[BX]=0x2000;
            c.mem[0]=0xf0;c.mem[1]=0x39;c.mem[2]=3;
            Jit jit(c,true,1,65536);jit.run();
            check(profile::data.lockedOpcodes[0x39]==1 && profile::data.interpretationReasons[0x39][unsigned(profile::InterpretationReason::Declined)]==1,
                  "LOCK-prefixed declined comparison attribution");
            c.ip=1;c.step();check(profile::data.lockedOpcodes[0x39]==1,"LOCK prefix leaked to next instruction");
        }
        for(bool cached:{false,true}) {
            CPU c(65536);c.s[CS].big=true;c.r[BX]=0x2000;
            c.mem[0]=0xe9;c.mem[1]=11;
            c.mem[16]=0x89;c.mem[17]=3;c.mem[18]=0xe9;
            const uint32_t back=uint32_t(-23);std::memcpy(c.mem.data()+19,&back,4);
            unsigned writes=0;c.watchStart=0x2000;c.watchLength=4;
            c.watchWrite=[&](uint32_t,uint32_t,int){++writes;};
            Jit jit(c,true,0,65536);jit.run();jit.run(); // Populate both entries.
            begin();writes=0;c.ip=cached?0:16;
            if(cached)jit.runBatch();else jit.run();
            check(writes==1 && profile::data.interpretationReasons[0x89][unsigned(profile::InterpretationReason::Replay)]==1,
                  "direct/cached replay dispatch reason");
            check(profile::interpretationReason==profile::InterpretationReason::OutsideJit,"replay reason leaked");
        }
#endif
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
