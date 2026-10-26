#include "hvf_machine.hpp"
#include "arm_virt.hpp"
#include "frontend_spice.hpp"
#include "arm_vmbus.hpp"
#include "machine_profile.hpp"
#include "hyperv_kvp_roundtrip.hpp"
#include <array>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <source_location>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <csignal>
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#include <sys/resource.h>
#include <mach/mach_time.h>
using pc::HvfMachine;
namespace {
volatile std::sig_atomic_t interrupted = 0;
void interrupt(int) { interrupted = 1; }
volatile std::sig_atomic_t guestShutdownRequested = 0;
void requestGuestShutdown(int) { guestShutdownRequested = 1; }
volatile std::sig_atomic_t captureRequested = 0;
void requestCapture(int) { captureRequested = 1; }
class SerialTerminal {
    termios previous{};
    bool changed = false;
  public:
    explicit SerialTerminal(bool enable) {
        if (enable && isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &previous) == 0) {
            auto raw = previous; cfmakeraw(&raw);
            changed = tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0;
        }
    }
    ~SerialTerminal() { if (changed) tcsetattr(STDIN_FILENO, TCSANOW, &previous); }
};
unsigned checks = 0;
void require(bool condition, const char *message) {
    ++checks;
    if (!condition)
        throw std::runtime_error(message);
}
template <class F> void rejects(F action, const char *message) {
    bool rejected = false;
    try { action(); } catch (const std::runtime_error &) { rejected = true; }
    require(rejected, message);
}
template <size_t N> void code(HvfMachine &vm, const std::array<uint32_t, N> &words) {
    vm.writeMemory(HvfMachine::ramBase,
                   {reinterpret_cast<const uint8_t *>(words.data()), sizeof(words)});
    vm.start(HvfMachine::ramBase);
}
HvfMachine::Exit hvc(HvfMachine &vm, std::source_location caller = std::source_location::current()) {
    auto exit = vm.run();
    // A watchdog request racing a completed exit may leave one pending cancel.
    for (unsigned retry = 0; retry < 2 && exit.reason == HvfMachine::ExitReason::Canceled; ++retry)
        exit = vm.run();
    std::ostringstream diagnostic;
    diagnostic << "Expected an AArch64 HVC exit at test line " << caller.line()
               << ": reason=" << unsigned(exit.reason) << " pc=0x" << std::hex << exit.pc
               << " syndrome=0x" << exit.syndrome;
    require(exit.reason == HvfMachine::ExitReason::Exception && exit.exceptionClass() == 0x16,
            diagnostic.str().c_str());
    return exit;
}
void benchmarkWatchdog() {
    HvfMachine vm;
    code(vm, std::array<uint32_t, 1>{0xd4000002}); // Repeat one immediate HVC exit.
    auto cpuSeconds = [] {
        rusage usage{};
        if (getrusage(RUSAGE_SELF, &usage)) throw std::runtime_error("getrusage failed");
        return double(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) +
            double(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1000000;
    };
    for (unsigned trial = 0; trial < 3; ++trial) {
        constexpr unsigned runs = 100000;
        unsigned canceled = 0;
        auto cpu = cpuSeconds();
        auto start = std::chrono::steady_clock::now();
        for (unsigned i = 0; i < runs;) {
            vm.setPC(HvfMachine::ramBase);
            auto exit = vm.run(std::chrono::milliseconds(16));
            if (exit.reason == HvfMachine::ExitReason::Canceled) { ++canceled; continue; }
            require(exit.exceptionClass() == 0x16, "Watchdog benchmark did not exit at HVC");
            ++i;
        }
        auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        std::cout << "{\"benchmark\":\"watchdog-exits\",\"trial\":" << trial
                  << ",\"runs\":" << runs << ",\"canceled\":" << canceled
                  << ",\"wallSeconds\":" << elapsed << ",\"cpuSeconds\":" << cpuSeconds() - cpu << "}\n";
    }
}
void selfTest() {
    // Invalid configuration must fail before claiming the process-wide VM.
    rejects([] { HvfMachine invalid(123); }, "Unaligned memory accepted");
    {
        HvfMachine vm(size_t(8) * 1024 * 1024 * 1024);
        code(vm, std::array<uint32_t, 3>{0xb9000020, 0xb9400022, 0xd4000002});
        rejects([&] { vm.sendSoftwareInterrupt(1, 1); }, "SGI accepted without a GIC");
        vm.setReg(0, 0x12345678);
        vm.setReg(1, HvfMachine::ramBase + size_t(8) * 1024 * 1024 * 1024 - 4);
        hvc(vm);
        require(vm.reg(2) == 0x12345678, "High guest RAM load/store failed");
    }
    for (unsigned iteration = 0; iteration < 2; ++iteration) {
        HvfMachine vm;
        rejects([] { HvfMachine second; }, "Second process-wide VM unexpectedly created");
        constexpr uint64_t loops = 10000000;
        // add x0,x0,#1; subs x1,x1,#1; b.ne -8; str x0,[x2]; ldr x3,[x2]; hvc #0
        code(vm, std::array<uint32_t, 6>{0x91000400, 0xf1000421, 0x54ffffc1,
                                        0xf9000040, 0xf9400043, 0xd4000002});
        vm.setReg(0, 0);
        vm.setReg(1, loops);
        vm.setReg(2, HvfMachine::ramBase + 0x10000);
        auto started = std::chrono::steady_clock::now();
        hvc(vm);
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now() - started).count();
        require(vm.reg(0) == loops && vm.reg(1) == 0 && vm.reg(3) == loops,
                "Guest arithmetic or RAM load failed");
        uint64_t stored = 0;
        vm.readMemory(HvfMachine::ramBase + 0x10000,
                      {reinterpret_cast<uint8_t *>(&stored), sizeof(stored)});
        require(stored == loops, "Guest store not visible in host RAM");
        require(vm.exits() == 1, "Compute loop caused unexpected guest/host exits");
        std::cout << "{\"test\":\"compute-ram\",\"iteration\":" << iteration
                  << ",\"guestInstructions\":" << loops * 3 + 3
                  << ",\"exits\":" << vm.exits() << ",\"wallNs\":" << ns << "}\n";

        // str w0,[x1]; ldr w2,[x1]; hvc #0. Unmapped IPA is a fake device.
        code(vm, std::array<uint32_t, 3>{0xb9000020, 0xb9400022, 0xd4000002});
        vm.setReg(0, 0x12345678);
        vm.setReg(1, 0x10000000);
        auto store = vm.run();
        require(store.reason == HvfMachine::ExitReason::Exception &&
                    store.exceptionClass() == 0x24 && store.physicalAddress == 0x10000000,
                "Expected stage-2 data abort at MMIO address");
        require((store.syndrome & (1ull << 24)) && (store.syndrome & (1ull << 6)) &&
                    ((store.syndrome >> 22) & 3) == 2 && ((store.syndrome >> 16) & 31) == 0,
                "MMIO store syndrome did not identify a 32-bit X0 write");
        require(vm.reg(0) == 0x12345678, "MMIO write payload incorrect");
        vm.setPC(store.pc + 4); // Device accepts the write; retire trapped store.
        auto load = vm.run();
        require(load.reason == HvfMachine::ExitReason::Exception &&
                    load.exceptionClass() == 0x24 && load.physicalAddress == 0x10000000,
                "Expected MMIO read exit");
        require((load.syndrome & (1ull << 24)) && !(load.syndrome & (1ull << 6)) &&
                    ((load.syndrome >> 16) & 31) == 2,
                "MMIO load syndrome did not identify X2 read");
        vm.setReg(2, 0xaabbccdd); // Device response, zero-extended for LDR W2.
        vm.setPC(load.pc + 4);
        hvc(vm);
        require(vm.reg(2) == 0xaabbccdd, "MMIO response lost on resume");

        // b . : watchdog must stop a guest that never traps voluntarily.
        code(vm, std::array<uint32_t, 1>{0x14000000});
        auto before = std::chrono::steady_clock::now();
        auto canceled = vm.run(std::chrono::milliseconds(20));
        require(canceled.reason == HvfMachine::ExitReason::Canceled,
                "Watchdog did not interrupt infinite loop");
        require(std::chrono::steady_clock::now() - before < std::chrono::seconds(5),
                "Watchdog exceeded cancellation bound");
        code(vm, std::array<uint32_t, 1>{0xd4000002});
        hvc(vm); // Prove canceled vCPU can resume executing new code.

        std::array<uint8_t, 8> bytes{};
        rejects([&] { vm.readMemory(HvfMachine::ramBase - 1, bytes); }, "Read below RAM accepted");
        rejects([&] { vm.writeMemory(std::numeric_limits<uint64_t>::max(), bytes); },
                "Overflowing write accepted");
        rejects([&] { vm.setReg(31, 0); }, "Invalid register accepted");
        rejects([&] { vm.start(HvfMachine::ramBase + 1); }, "Misaligned entry accepted");
        rejects([&] { vm.run(std::chrono::milliseconds(0)); }, "Zero timeout accepted");
        bool wrongThreadRejected = false;
        std::thread other([&] {
            try { vm.reg(0); } catch (const std::runtime_error &) { wrongThreadRejected = true; }
        });
        other.join();
        require(wrongThreadRejected, "Cross-thread vCPU operation accepted");
    } // Repeated create/destroy proves cleanup released the process VM.
    {
        HvfMachine vm(2 * 1024 * 1024, true);
        const std::array<uint32_t, 2> rom{0xb9000020, 0xd4000002}; // str w0,[x1]; hvc #0
        vm.mapROM(0x20000000, {reinterpret_cast<const uint8_t *>(rom.data()), sizeof(rom)});
        rejects([&] { vm.mapROM(0x20000000, {reinterpret_cast<const uint8_t *>(rom.data()), sizeof(rom)}); },
                "Overlapping ROM accepted");
        rejects([&] { vm.mapROM(HvfMachine::ramBase, {reinterpret_cast<const uint8_t *>(rom.data()), sizeof(rom)}); },
                "ROM over RAM accepted");
        vm.start(0x20000000);
        vm.setReg(0, 0); vm.setReg(1, 0x20000000);
        auto exit = vm.run();
        require(exit.reason == HvfMachine::ExitReason::Exception && exit.exceptionClass() == 0x24 &&
                    exit.physicalAddress == 0x20000000 && (exit.syndrome & (1ull << 6)),
                "Guest could write ROM");
        uint32_t word = 0;
        vm.readMemory(0x20000000, {reinterpret_cast<uint8_t *>(&word), sizeof(word)});
        require(word == rom[0], "ROM changed after guest write fault");
        vm.setPC(exit.pc + 4); hvc(vm);
        require((vm.systemReg(0xc005) & 0xffffff) == 0, "Unexpected GIC CPU affinity");
    }
    {
        HvfMachine vm(2 * 1024 * 1024, true);
        // Framework GIC mode handles the exposed PMU cycle counter. Verify that
        // neither read requires an extra userspace exit; only HVC should exit.
        code(vm, std::array<uint32_t, 6>{0xd53b9d00, 0xd2807d02, 0xf1000442,
                                       0x54ffffe1, 0xd53b9d01, 0xd4000002});
        hvc(vm);
        require(vm.reg(1) > vm.reg(0), "Framework PMU counter did not advance");
        require(vm.exits() == 1, "PMU cycle reads caused userspace exits");
        require(((vm.systemReg(0xc028) >> 8) & 15) == 1, "PMUv3 feature was not exposed");
    }
    {
        uint64_t sleep = 123456; // Sleep before VM creation must not be added.
        HvfMachine vm(2 * 1024 * 1024, false, [&] { return sleep; });
        const std::array<uint32_t, 3> counter{0xd53be040, 0xd53be001, 0xd4000002};
        // MRS CNTVCT_EL0, MRS CNTFRQ_EL0, HVC. Check the real guest counter
        // against host absolute-time brackets with injected system-sleep gaps.
        code(vm, counter);
        auto absoluteBefore = mach_absolute_time();
        hvc(vm);
        auto absoluteAfter = mach_absolute_time();
        auto previous = vm.reg(0), frequency = vm.reg(1);
        require(frequency != 0, "Guest counter frequency is zero");
        for (unsigned sample = 0; sample < 4; ++sample) {
            if (sample == 0) sleep = 123456 + frequency / 1000; // Ignore sampling jitter.
            if (sample == 1) sleep = 123456 + 2 * frequency;
            if (sample == 3) sleep = 123456 + frequency; // Cannot rewind time.
            std::this_thread::sleep_for(std::chrono::milliseconds(1100));
            vm.start(HvfMachine::ramBase);
            auto before = mach_absolute_time();
            hvc(vm);
            auto after = mach_absolute_time();
            auto elapsed = vm.reg(0) - previous;
            auto correction = sample == 1 ? 2 * frequency : 0;
            require(elapsed >= before - absoluteAfter + correction &&
                    elapsed <= after - absoluteBefore + correction,
                    "Guest counter lost sleep time, repeated correction or moved backwards");
            previous = vm.reg(0); absoluteBefore = before; absoluteAfter = after;
        }
    }
    {
        HvfMachine vm(2 * 1024 * 1024, true, {}, 2);
        // Both VPs repeatedly call a shared code page, exiting after each call.
        // Wait for one host mailbox phase per call: a fast VP must not keep
        // exiting and canceling the other VP before its first instruction.
        code(vm, std::array<uint32_t, 9>{0xd2800024, 0x91400405, 0xf94000a6,
            0xeb0400df, 0x54ffffa1, 0xd63f0000, 0x91000484, 0xd4000002, 0x17fffff9});
        const auto function = HvfMachine::ramBase + 0x1000;
        const std::array<uint32_t, 2> oldCode{0xd2800023, 0xd65f03c0}; // mov x3,#1; ret
        vm.writeMemory(function, {reinterpret_cast<const uint8_t *>(oldCode.data()), sizeof(oldCode)});
        vm.setReg(0, function);
        require(vm.startCPU(1, HvfMachine::ramBase, function) == 0, "Barrier secondary start failed");
        uint64_t phase = 0;
        auto observe = [&](uint64_t expected) {
            ++phase;
            vm.writeMemory(function + 0x1000, {reinterpret_cast<const uint8_t *>(&phase), sizeof(phase)});
            unsigned seen = 0;
            for (unsigned attempt = 0; attempt < 16 && seen != 3; ++attempt) {
                auto exit = vm.run(std::chrono::milliseconds(100));
                if (exit.reason == HvfMachine::ExitReason::Canceled) continue;
                require(exit.exceptionClass() == 0x16, "Unexpected code synchronization guest exit");
                require(vm.reg(3) == expected, "Target VP executed stale instructions");
                seen |= 1u << exit.cpu;
            }
            require(seen == 3, "Both barrier VPs did not execute shared code");
        };
        observe(1);
        auto pc0 = vm.cpuPC(0), pc1 = vm.cpuPC(1);
        auto selected = vm.currentCPU();
        std::array<uint64_t, 31> registers;
        for (unsigned i = 0; i < 31; ++i) registers[i] = vm.reg(i);
        const auto sctlr = vm.systemReg(0xc080), vbar = vm.systemReg(0xc600);
        const uint32_t newInstruction = 0xd2800043; // mov x3,#2
        vm.writeMemory(function, {reinterpret_cast<const uint8_t *>(&newInstruction), 4});
        vm.synchronizeInstructionContext(3);
        require(vm.instructionContextBarriers(0) == 1 && vm.instructionContextBarriers(1) == 1,
                "Real instruction barriers did not execute on both VPs");
        require(vm.currentCPU() == selected && vm.cpuPC(0) == pc0 && vm.cpuPC(1) == pc1,
                "Instruction barrier changed guest PCs or selected CPU");
        for (unsigned i = 0; i < 31; ++i)
            require(vm.reg(i) == registers[i], "Instruction barrier changed guest GPRs");
        require(vm.systemReg(0xc080) == sctlr && vm.systemReg(0xc600) == vbar,
                "Instruction barrier did not restore MMU/vector state");
        observe(2);
        vm.synchronizeInstructionContext(2);
        require(vm.instructionContextBarriers(0) == 1 && vm.instructionContextBarriers(1) == 2,
                "Instruction barrier ignored the target mask");
        vm.synchronizeInstructionContext(0);
        rejects([&] { vm.synchronizeInstructionContext(4); }, "Invalid barrier CPU mask accepted");
        require(vm.instructionContextBarriers(1) == 2, "Empty/rejected barrier changed counters");
        // Unexpected EL1 exceptions must hit the private vector guard and
        // restore guest exception registers before reporting failure.
        const uint32_t breakpoint = 0xd4200000, restoreBarrier = 0xd5033f9f;
        vm.writeMemory(0x3fff0000, {reinterpret_cast<const uint8_t *>(&breakpoint), 4});
        const auto oldElr = vm.systemReg(0xc201), oldEsr = vm.systemReg(0xc290);
        for (auto mask : {1u, 2u})
            rejects([&] { vm.synchronizeInstructionContext(mask); }, "Faulting instruction barrier acknowledged");
        require(vm.systemReg(0xc201) == oldElr && vm.systemReg(0xc290) == oldEsr,
                "Faulting instruction barrier corrupted exception context");
        vm.writeMemory(0x3fff0000, {reinterpret_cast<const uint8_t *>(&restoreBarrier), 4});
        // Exercise real cancellation and rollback on each vCPU owner thread.
        const uint32_t infiniteLoop = 0x14000000, barrier = 0xd5033f9f;
        vm.writeMemory(0x3fff0000, {reinterpret_cast<const uint8_t *>(&infiniteLoop), 4});
        for (auto mask : {1u, 2u})
            rejects([&] { vm.synchronizeInstructionContext(mask); }, "Incomplete instruction barrier acknowledged");
        vm.writeMemory(0x3fff0000, {reinterpret_cast<const uint8_t *>(&barrier), 4});
        vm.synchronizeInstructionContext(3);
        require(vm.instructionContextBarriers(0) == 2 && vm.instructionContextBarriers(1) == 3,
                "Barrier did not recover after canceled maintenance");
        observe(2);
    }
    {
        HvfMachine vm(2 * 1024 * 1024, true, {}, 2);
        // Both VPs install shared 4 KiB translation tables (39-bit VA,
        // TTBR0 only, MAIR normal WBWA), then load one alias per mailbox phase.
        // There is no guest TLBI after changing the alias PTE.
        code(vm, std::array<uint32_t, 24>{
            0xd2a80401, 0x91402805, 0xd2800026, 0xd2801fe3, // alias, mailbox, phase, MAIR
            0xd518a203, 0xd5182000, 0xd286a323, 0xf2a01003, // MAIR/TTBR0, TCR=0x803519
            0xd5182043, 0xd5033f9f, 0xd5033fdf, 0xd5381003, // TCR, DSB/ISB, SCTLR
            0xb2400063, 0xb27e0063, 0xb2740063, 0xd5181003, // enable MMU/data/icache
            0xd5033fdf, 0xf94000a7, 0xeb0600ff, 0x54ffffc1, // ISB; wait for host phase
            0xf9400024, 0x910004c6, 0xd4000002, 0x17fffffa}); // load alias; next phase; HVC; repeat
        constexpr auto base = HvfMachine::ramBase;
        auto write64 = [&](uint64_t at, uint64_t value) {
            vm.writeMemory(at, {reinterpret_cast<const uint8_t *>(&value), sizeof(value)});
        };
        auto table = [&](uint64_t at, const std::array<uint64_t, 512> &entries) {
            vm.writeMemory(at, {reinterpret_cast<const uint8_t *>(entries.data()), sizeof(entries)});
        };
        std::array<uint64_t, 512> entries{};
        entries[1] = (base + 0x11000) | 3; table(base + 0x10000, entries); // L1
        entries.fill(0);
        entries[0] = (base + 0x12000) | 3; entries[1] = (base + 0x13000) | 3;
        table(base + 0x11000, entries); // L2: identity and alias L3 tables
        for (unsigned i = 0; i < 512; ++i) entries[i] = (base + i * 4096) | 0x703;
        table(base + 0x12000, entries);
        write64(base + 0x13000, (base + 0x18000) | 0x703); // VA 0x40200000 -> data A
        write64(base + 0x18000, 11); write64(base + 0x19000, 22);
        vm.setReg(0, base + 0x10000);
        require(vm.startCPU(1, base, base + 0x10000) == 0, "TLB secondary start failed");
        uint64_t phase = 0;
        auto observe = [&](uint64_t expected) {
            write64(base + 0x1a000, ++phase);
            unsigned seen = 0;
            for (unsigned attempt = 0; attempt < 16 && seen != 3; ++attempt) {
                auto exit = vm.run(std::chrono::milliseconds(100));
                if (exit.reason == HvfMachine::ExitReason::Canceled) continue;
                require(exit.exceptionClass() == 0x16, "Unexpected translated guest exit");
                require(vm.reg(4) == expected, "Guest observed a stale/wrong translation");
                require(vm.systemReg(0xc080) & 1, "Translation test did not enable the MMU");
                seen |= 1u << exit.cpu;
            }
            require(seen == 3, "Both VPs did not observe the translation");
        };
        observe(11);
        auto tcr = vm.systemReg(0xc102), ttbr = vm.systemReg(0xc100);
        write64(base + 0x13000, (base + 0x19000) | 0x703); // Change alias to data B.
        vm.flushTranslationContext(3);
        require(vm.translationContextFlushes(0) == 1 && vm.translationContextFlushes(1) == 1,
                "Translation maintenance did not execute on both VPs");
        require(vm.instructionContextBarriers(0) == 0 && vm.instructionContextBarriers(1) == 0,
                "Translation flushes were mislabeled as SyncContext requests");
        require(vm.systemReg(0xc102) == tcr && vm.systemReg(0xc100) == ttbr,
                "Translation flush changed guest translation controls");
        observe(22);
        vm.flushTranslationContext(2);
        require(vm.translationContextFlushes(0) == 1 && vm.translationContextFlushes(1) == 2,
                "Translation maintenance ignored its target mask");
        vm.flushTranslationContext(0);
        rejects([&] { vm.flushTranslationContext(4); }, "Invalid translation flush mask accepted");
        // A flush that does not reach its matching completion sentinel must fail.
        uint32_t wrong = 0xd41fffa2, correct = 0xd41fffc2;
        vm.writeMemory(0x3fff0110, {reinterpret_cast<const uint8_t *>(&wrong), 4});
        rejects([&] { vm.flushTranslationContext(2); }, "Incomplete translation maintenance acknowledged");
        require(vm.translationContextFlushes(1) == 2, "Failed flush incremented completion counter");
        vm.writeMemory(0x3fff0110, {reinterpret_cast<const uint8_t *>(&correct), 4});
        vm.flushTranslationContext(3);
        observe(22);
    }
    {
        HvfMachine vm(2 * 1024 * 1024, true, {}, 2);
        // Configure Group-1 SGIs in each GICR frame and CPU interface, then
        // read IAR1 once per host phase. DAIF stays masked: software explicitly
        // acknowledges the interrupt, making routing and EOI observable.
        code(vm, std::array<uint32_t, 42>{
            0xd53800a0, 0x92401c00, 0xd2a10161, 0x8b004421, // MPIDR -> GICR SGI frame
            0x12800002, 0xb9008022, 0x12800002, 0xb9010022, // Group 1, enable SGIs and PPIs
            0xd2a10003, 0x52800262, 0xb9000062, 0xd2800022, // enable distributor
            0xd518cca2, 0xd5033fdf, 0xd2801fe2, 0xd5184602, // SRE, PMR=255
            0xd2800022, 0xd518cce2, 0xd5033fdf, 0xd2a80025, // Group-1 enable, mailbox
            0xd2800028, 0xf94000a9, 0xeb08013f, 0x54ffffc1, // wait for phase
            0xf100251f, 0x54000120, 0xd538cc04, 0xd4000002, // phase 9 -> IRQ wait; otherwise IAR/HVC
            0xf10ff09f, 0x54000062, 0xd518cc24, 0xd5033fdf, // skip spurious EOI
            0x91000508, 0x17fffff4, 0xd282000a, 0xf2a8000a, // next phase; vector base
            0xd518c00a, 0xd5033fdf, 0xd50342ff, 0xd5033fdf, // VBAR; unmask IRQ
            0xd503207f, 0x17ffffff}); // WFI; repeat
        const auto vectors = HvfMachine::ramBase + 0x1000;
        const std::array<uint32_t, 5> irqHandler{0xd538cc04, 0xd518cc24, 0xd5033fdf, 0xd4000aa2, 0xd69f03e0};
        vm.writeMemory(vectors + 0x280,
            {reinterpret_cast<const uint8_t *>(irqHandler.data()), sizeof(irqHandler)});
        require(vm.startCPU(1, HvfMachine::ramBase, 0) == 0, "SGI secondary start failed");
        uint64_t phase = 0;
        auto acknowledge = [&](uint64_t primary, uint64_t secondary) {
            ++phase;
            vm.writeMemory(HvfMachine::ramBase + 0x10000,
                {reinterpret_cast<const uint8_t *>(&phase), sizeof(phase)});
            unsigned seen = 0;
            for (unsigned attempt = 0; attempt < 16 && seen != 3; ++attempt) {
                auto exit = vm.run(std::chrono::milliseconds(100));
                if (exit.reason == HvfMachine::ExitReason::Canceled) continue;
                require(exit.exceptionClass() == 0x16, "Unexpected SGI guest exit");
                require(vm.reg(4) == (exit.cpu ? secondary : primary), "SGI delivered to wrong CPU or vector");
                seen |= 1u << exit.cpu;
            }
            require(seen == 3, "Both VPs did not acknowledge the SGI phase");
        };
        acknowledge(1023, 1023); // Initial CPU interface has no pending interrupt.
        vm.sendSoftwareInterrupt(2, 5);
        vm.sendSoftwareInterrupt(2, 5); // Pending SGIs coalesce, unlike request counters.
        acknowledge(1023, 5);
        vm.sendSoftwareInterrupt(3, 7);
        vm.sendSoftwareInterrupt(2, 9); // Must preserve SGI 7 while pending SGI 9.
        const auto gic0 = vm.privateInterruptState(0), gic1 = vm.privateInterruptState(1);
        require(gic0.available && gic1.available && (gic0.enabled & (1u << 7)) &&
                (gic0.pending & (1u << 7)) && (gic1.pending & (1u << 7)) &&
                (gic1.pending & (1u << 9)), "GIC snapshot missed pending SGIs");
        require(vm.privateInterruptState(0).pending == gic0.pending &&
                vm.privateInterruptState(1).pending == gic1.pending,
                "GIC snapshot consumed a pending interrupt");
        rejects([&] { vm.privateInterruptState(2); }, "Out-of-range GIC snapshot accepted");
        acknowledge(7, 7);
        acknowledge(1023, 9);
        acknowledge(1023, 1023); // EOI removed both requests; no duplicate SGI 5.
        require(vm.softwareInterruptRequests(0) == 1 && vm.softwareInterruptRequests(1) == 4,
                "Software interrupt request counters are incorrect");
        vm.sendSoftwareInterrupt(0, 15);
        rejects([&] { vm.sendSoftwareInterrupt(4, 5); }, "Invalid SGI target accepted");
        rejects([&] { vm.sendSoftwareInterrupt(3, 16); }, "Non-SGI vector accepted");
        require(vm.softwareInterruptRequests(0) == 1 && vm.softwareInterruptRequests(1) == 4,
                "Empty/rejected SGI request changed counters");
        pc::ArmSynic synic(vm.ram(), HvfMachine::ramBase, 2);
        synic.set_interrupt_sender([&](uint64_t targets, unsigned vector) {
            vm.sendPrivateInterrupt(targets, vector); return true;
        });
        for (unsigned cpu = 0; cpu < 2; ++cpu) {
            require(synic.write_vp_register(0xa0010, cpu, 1) == 0 &&
                    synic.write_vp_register(0xa0012, cpu, HvfMachine::ramBase + 0x18001 + cpu * 0x2000) == 0 &&
                    synic.write_vp_register(0xa0013, cpu, HvfMachine::ramBase + 0x19001 + cpu * 0x2000) == 0 &&
                    synic.write_vp_register(0xa0002, cpu, 16) == 0 && synic.write_vp_register(0xa0003, cpu, 31) == 0,
                    "Native SynIC configuration failed");
        }
        bool newly = false;
        require(synic.signal_event(1, 2, 2047, newly) == 0 && newly &&
                vm.ram()[0x1a000 + 767] == 128, "Native SynIC flag was not deposited");
        acknowledge(1023, 16);
        pc::ArmHyperVTimers syntheticTimers(2);
        syntheticTimers.setDelivery([&](unsigned cpu, unsigned timer, uint64_t config, uint64_t expiration, uint64_t now) {
            if (config & 0x1000) {
                require(cpu == 1 && timer == 1 && expiration == 200 && now == 200, "Native direct timer expired incorrectly");
                vm.sendPrivateInterrupt(1ull << cpu, unsigned((config >> 4) & 31));
                return true;
            }
            const std::array<uint64_t, 3> message{timer, expiration, now};
            require(timer == 0 && expiration == 100 && now == 100, "Native timer expired early");
            return synic.try_post_message(cpu, 3, 0x80000010, 0,
                {reinterpret_cast<const uint8_t *>(message.data()), sizeof(message)}) == 0;
        });
        for (unsigned cpu = 0; cpu < 2; ++cpu)
            require(syntheticTimers.write(0xb0001, cpu, 100, 0) == 0 &&
                    syntheticTimers.write(0xb0000, cpu, 0x30001, 0) == 0,
                    "Native timer programming failed");
        syntheticTimers.service(99);
        require(syntheticTimers.delivered() == 0, "Native timer fired before its deadline");
        syntheticTimers.service(100);
        require(syntheticTimers.delivered() == 2, "Native timer messages were not delivered");
        acknowledge(31, 31);
        require(vm.privateInterruptRequests(0) == 2 && vm.privateInterruptRequests(1) == 6,
                "PPI target request counters are incorrect");
        rejects([&] { vm.sendPrivateInterrupt(3, 32); }, "Shared interrupt accepted as private");
        rejects([&] { vm.sendPrivateInterrupt(4, 16); }, "Invalid PPI target accepted");
        require(vm.softwareInterruptRequests(0) == 1 && vm.softwareInterruptRequests(1) == 4,
                "PPI requests changed Hyper-V IPI counters");
        require(syntheticTimers.write(0xb0003, 1, 200, 100) == 0 &&
                syntheticTimers.write(0xb0002, 1, 0x1111, 100) == 0,
                "Native direct timer programming failed");
        syntheticTimers.service(200);
        acknowledge(1023, 17);
        require(syntheticTimers.delivered() == 3 && vm.privateInterruptRequests(1) == 7,
                "Native direct timer did not pend its target PPI");
        vm.sendSoftwareInterrupt(2, 0); // The SGI ID observed in real Windows.
        ++phase;
        vm.writeMemory(HvfMachine::ramBase + 0x10000,
            {reinterpret_cast<const uint8_t *>(&phase), sizeof(phase)});
        bool handled = false;
        for (unsigned attempt = 0; attempt < 16 && !handled; ++attempt) {
            auto exit = vm.run(std::chrono::milliseconds(100));
            if (exit.reason == HvfMachine::ExitReason::Canceled) continue;
            require(exit.exceptionClass() == 0x16 && (exit.syndrome & 0xffff) == 0x55 &&
                    exit.cpu == 1 && exit.pc == vectors + 0x290 && vm.reg(4) == 0,
                    "Target VP did not execute its IRQ handler for SGI 0");
            handled = true;
        }
        require(handled, "SGI 0 IRQ handler did not execute");
        auto resumed = vm.run(std::chrono::milliseconds(2));
        require(resumed.reason == HvfMachine::ExitReason::Canceled &&
                vm.cpuPC(1) >= HvfMachine::ramBase + 0x88 && vm.cpuPC(1) < HvfMachine::ramBase + 0xa8,
                "IRQ handler did not ERET to the interrupted guest");
        require(vm.softwareInterruptRequests(0) == 1 && vm.softwareInterruptRequests(1) == 5,
                "Vectored IRQ changed the wrong request counter");
    }
    for (bool multiclient : {false, true}) {
        // An actual guest submits fast SMCCC SignalEvent and memory legacy
        // PostMessage. Both host endpoints route to SynIC SGI0 or Windows-style PPI31; guest
        // IAR/EOI and the shared event/message pages must show real effects.
        HvfMachine vm(2 * 1024 * 1024, true);
        pc::ArmHyperV hyperv(true, vm.ram(), HvfMachine::ramBase, 1);
        pc::ArmHyperV::Registers registers{};
        registers[0] = 0x46000001; registers[1] = 0x100010051;
        registers[2] = UINT64_MAX; registers[3] = 0xfffffffe;
        registers[4] = 0x90002; registers[6] = 0x5043454d55;
        require(hyperv.call(0, registers, 0) && registers[0] == (1ull << 32), "Connection fixture identity failed");
        auto &synic = hyperv.synicController();
        require(synic.write_vp_register(0xa0010, 0, 1) == 0 &&
                synic.write_vp_register(0xa0012, 0, HvfMachine::ramBase + 0x19001) == 0 &&
                synic.write_vp_register(0xa0013, 0, HvfMachine::ramBase + 0x1a001) == 0 &&
                synic.write_vp_register(0xa0002, 0, multiclient ? 0 : 31) == 0, "Connection fixture SynIC setup failed");
        hyperv.setPrivateInterruptSender([&](uint64_t targets, unsigned vector) {
            vm.sendPrivateInterrupt(targets, vector); return true;
        });
        require(hyperv.bindEventConnection(42, 10, [&](unsigned, uint16_t relative) {
            bool newly = false; return synic.signal_event(0, 2, 100 + relative, newly);
        }), "Native event endpoint binding failed");
        hyperv.advertiseSynic(true);
        hyperv.enableVmbus();
        std::array<uint8_t, 256> packet{};
        packet[0] = multiclient ? 4 : 1; packet[4] = 0xa5; packet[8] = 1; packet[12] = 40;
        packet[16] = 14; packet[26] = 6; packet[32] = multiclient ? 2 : 0; // Copper contact, VP0.
        vm.writeMemory(HvfMachine::ramBase + 0x18000, packet);
        // Group-1 GIC setup, fast event call, IAR/EOI, slow message call,
        // IAR/EOI, then success/failure HVC markers (DAIF stays masked).
        code(vm, std::array<uint32_t, 47>{
            0xd53800a0, 0x92401c00, 0xd2a10161, 0x8b004421, 0x12800002, 0xb9008022,
            0xb9010022, 0xd2a10003, 0x52800262, 0xb9000062, 0xd2800022, 0xd518cca2,
            0xd5033fdf, 0xd2801fe2, 0xd5184602, 0xd2800022, 0xd518cce2, 0xd5033fdf,
            0xd2800020, 0xf2a8c000, 0xd2800ba1, 0xf2a00021, 0xd2800542, 0xf2c00062,
            0x92800003, 0xd4000002, 0xb5000260, 0xd538cc04, multiclient ? 0xf100009fu : 0xf1007c9fu, 0x54000201,
            0xd518cc24, 0xd5033fdf, 0xd2800b80, 0xd2900001, 0xf2a80021, 0x92800002,
            0xd4000022, 0xb5000100, 0xd538cc04, multiclient ? 0xf100009fu : 0xf1007c9fu, 0x540000a1, 0xd518cc24,
            0xd5033fdf, 0xd4000aa2, 0x14000000, 0xd4000cc2, 0x14000000
        });
        bool finished = false;
        for (unsigned attempt = 0; attempt < 16 && !finished; ++attempt) {
            auto exit = vm.run(std::chrono::milliseconds(100));
            if (exit.reason == HvfMachine::ExitReason::Canceled) continue;
            require(exit.exceptionClass() == 0x16, "Unexpected native connection guest exit");
            auto immediate = unsigned(exit.syndrome & 0xffff);
            if (immediate <= 1) {
                for (unsigned i = 0; i < registers.size(); ++i) registers[i] = vm.reg(i);
                require(hyperv.call(immediate, registers, 0) && registers[0] == 0,
                        "Native connection hypercall failed");
                hyperv.service();
                for (unsigned i = 0; i < registers.size(); ++i) vm.setReg(i, registers[i]);
            } else {
                require(immediate == 0x55 && vm.reg(4) == (multiclient ? 0 : 31), "Native connection guest did not acknowledge both private interrupts");
                finished = true;
            }
        }
        require(finished && vm.privateInterruptRequests(0) == 2, "Native connection delivery did not complete");
        require(vm.ram()[0x19000 + 512 + 103 / 8] == 128 &&
                vm.ram()[0x1a000 + 512] == 1 && vm.ram()[0x1a000 + 512 + 4] == 20 &&
                vm.ram()[0x1a000 + 512 + 16] == 15 && vm.ram()[0x1a000 + 512 + 24] == 1 &&
                vm.ram()[0x1a000 + 512 + 28] == 1,
                "Native connection guest RAM effects are incorrect");
    }
    for (bool gic : {false, true}) {
        uint64_t sleep = 0;
        HvfMachine vm(2 * 1024 * 1024, gic, [&] { return sleep; });
        // Arm CNTV_CVAL ten seconds ahead, enable CNTV_CTL, HVC. With a GIC,
        // read the redistributor pending register; otherwise wait for HVF exit.
        code(vm, std::array<uint32_t, 11>{0xd53be040, 0xd53be001, 0xd2800143,
            0x9b030020, 0xd51be340, 0xd2800022, 0xd51be322, 0xd5033fdf,
            0xd4000002, gic ? 0xb9400080u : 0xd503207fu,
            gic ? 0xd4000002u : 0x14000000u});
        vm.setReg(4, 0x080b0200); // GICR_ISPENDR0
        hvc(vm);
        sleep = 20 * vm.reg(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        if (gic) {
            hvc(vm);
            require(vm.reg(0) & (1u << 27), "GIC virtual timer not pending after simulated sleep");
        } else {
            auto exit = vm.run(std::chrono::milliseconds(100));
            for (unsigned retry = 0; retry < 2 && exit.reason == HvfMachine::ExitReason::Canceled; ++retry)
                exit = vm.run(std::chrono::milliseconds(100));
            require(exit.reason == HvfMachine::ExitReason::VirtualTimer,
                    "Virtual timer did not expire after simulated system sleep");
        }
    }
    {
        HvfMachine vm(2 * 1024 * 1024, true, {}, 2);
        require(vm.cpuCount() == 2 && vm.cpuOnline(0) && !vm.cpuOnline(1), "SMP reset state is invalid");
        code(vm, std::array<uint32_t, 12>{0xd53800a1, 0x92401c21, 0xd2800022,
            0xf9000002, 0xd5033f9f, 0xd2400023, 0xcb010c04, 0x8b030c84,
            0xf9400085, 0xb4ffffe5, 0xd4000002, 0x14000000});
        auto mailbox = HvfMachine::ramBase + 0x10000;
        vm.setReg(0, mailbox);
        require(vm.startCPU(2, HvfMachine::ramBase, 0) == -2, "Invalid secondary CPU accepted");
        require(vm.startCPU(1, 3, 0) == -2, "Unaligned secondary entry accepted");
        require(vm.startCPU(1, HvfMachine::ramBase, mailbox + 8) == 0, "Secondary CPU start failed");
        require(vm.startCPU(1, HvfMachine::ramBase, 0) == -4, "Running CPU restarted");
        unsigned seen = 0;
        for (unsigned attempt = 0; seen != 3 && attempt < 8; ++attempt) {
            auto exit = vm.run(std::chrono::milliseconds(500));
            if (attempt == 0) require(exit.reason != HvfMachine::ExitReason::Canceled,
                "CPUs could not pass a shared-memory barrier concurrently");
            if (exit.reason == HvfMachine::ExitReason::Canceled) continue;
            require(exit.reason == HvfMachine::ExitReason::Exception && exit.exceptionClass() == 0x16,
                    "Unexpected SMP exit");
            require(vm.currentCPU() == exit.cpu && vm.reg(1) == exit.cpu, "Wrong secondary register context");
            require(vm.reg(0) == mailbox + 8 * exit.cpu, "CPU context argument was lost");
            seen |= 1u << exit.cpu;
            if (exit.cpu == 1) {
                require((vm.systemReg(0xc005) & 0xff) == 1, "Secondary MPIDR is incorrect");
                require(vm.stopCPU() == 0, "Secondary CPU_OFF failed");
            }
        }
        require(seen == 3 && !vm.cpuOnline(1), "SMP barrier or CPU_OFF failed");
        require(vm.startCPU(1, HvfMachine::ramBase, mailbox + 8) == 0, "CPU hotplug restart failed");
        bool restarted = false;
        for (unsigned attempt = 0; attempt < 8 && !restarted; ++attempt) {
            auto exit = vm.run(std::chrono::milliseconds(100));
            restarted = exit.reason == HvfMachine::ExitReason::Exception && exit.cpu == 1 && exit.exceptionClass() == 0x16;
        }
        require(restarted && vm.reg(0) == mailbox + 8, "CPU hotplug lost its context");
        require(vm.stopCPU() == 0, "Could not prepare idle-secondary test");
        const std::array<uint32_t, 2> idle{0xd503207f, 0x17ffffff}; // WFI; repeat.
        vm.writeMemory(HvfMachine::ramBase + 0x1000,
            {reinterpret_cast<const uint8_t *>(idle.data()), sizeof(idle)});
        code(vm, std::array<uint32_t, 3>{0x91000400, 0xf9000020, 0x17fffffe}); // Count/store/repeat.
        vm.setReg(0, 0); vm.setReg(1, mailbox + 32);
        require(vm.startCPU(1, HvfMachine::ramBase + 0x1000, 0) == 0, "Idle secondary start failed");
        vm.run(std::chrono::milliseconds(2));
        uint64_t first = 0, last = 0;
        vm.readMemory(mailbox + 32, {reinterpret_cast<uint8_t *>(&first), sizeof(first)});
        for (unsigned i = 0; i < 10; ++i) vm.run(std::chrono::milliseconds(2));
        vm.readMemory(mailbox + 32, {reinterpret_cast<uint8_t *>(&last), sizeof(last)});
        require(last > first, "Watchdog cancellations starved a CPU beside an idle secondary");
        vm.synchronizeInstructionContext(3);
        require(vm.instructionContextBarriers(0) == 1 && vm.instructionContextBarriers(1) == 1,
                "Instruction synchronization did not cover the WFI secondary");
    }
    {
        uint64_t sleep = 0;
        HvfMachine vm(2 * 1024 * 1024, true, [&] { return sleep; }, 2);
        // Each CPU reads the counter once per host-supplied mailbox phase.
        code(vm, std::array<uint32_t, 9>{0xd2800024, 0xf9400005, 0xeb0400bf,
            0x54ffffc1, 0xd53be041, 0xd53be002, 0x91000484, 0xd4000002, 0x17fffff9});
        auto mailbox = HvfMachine::ramBase + 0x10000;
        auto phase = [&](uint64_t value) {
            vm.writeMemory(mailbox, {reinterpret_cast<const uint8_t *>(&value), sizeof(value)});
        };
        phase(1); vm.setReg(0, mailbox);
        require(vm.startCPU(1, HvfMachine::ramBase, mailbox) == 0, "Clock-test CPU start failed");
        uint64_t frequency = 0;
        auto sample = [&] {
            std::array<uint64_t, 2> counters{}; unsigned seen = 0;
            for (unsigned attempt = 0; seen != 3 && attempt < 16; ++attempt) {
                auto exit = vm.run(std::chrono::milliseconds(100));
                if (exit.reason == HvfMachine::ExitReason::Canceled) continue;
                require(exit.reason == HvfMachine::ExitReason::Exception && exit.exceptionClass() == 0x16,
                        "Unexpected SMP clock-test exit");
                counters[exit.cpu] = vm.reg(1); seen |= 1u << exit.cpu;
                if (!frequency) frequency = vm.reg(2);
                require(vm.reg(2) == frequency && frequency != 0, "SMP counter frequencies differ");
            }
            require(seen == 3, "SMP clock sample incomplete");
            return counters;
        };
        auto before = sample(); sleep = 20 * frequency;
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        phase(2); auto after = sample();
        for (unsigned i = 0; i < 2; ++i)
            require(after[i] - before[i] >= sleep, "A secondary CPU lost host sleep time");
        require(std::max(after[0], after[1]) - std::min(after[0], after[1]) < frequency,
                "SMP virtual counters diverged after host sleep");
    }
    {
        HvfMachine vm;
        code(vm, std::array<uint32_t, 1>{0x14000000}); // uncooperative guest
        vm.requestExit();
        auto before = std::chrono::steady_clock::now();
        auto exit = vm.run(std::chrono::milliseconds(2000));
        require(exit.reason == HvfMachine::ExitReason::Canceled &&
                std::chrono::steady_clock::now() - before < std::chrono::milliseconds(500),
                "Readiness just before run was lost");
        std::thread notification([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            vm.requestExit();
        });
        before = std::chrono::steady_clock::now();
        exit = vm.run(std::chrono::milliseconds(2000));
        notification.join();
        require(exit.reason == HvfMachine::ExitReason::Canceled &&
                std::chrono::steady_clock::now() - before < std::chrono::milliseconds(500),
                "Readiness did not interrupt active guest CPUs");
        code(vm, std::array<uint32_t, 1>{0xd4000002});
        // A notification during run may leave a harmless latched cancellation.
        do { exit = vm.run(std::chrono::milliseconds(100)); }
        while (exit.reason == HvfMachine::ExitReason::Canceled);
        require(exit.exceptionClass() == 0x16, "Guest did not resume after readiness wakeup");
    }
    {
        HvfMachine vm;
        auto timedLoop = [&](unsigned budget, unsigned lower, unsigned upper) {
            code(vm, std::array<uint32_t, 1>{0x14000000});
            auto start = std::chrono::steady_clock::now();
            auto exit = vm.run(std::chrono::milliseconds(budget));
            auto elapsed = std::chrono::steady_clock::now() - start;
            require(exit.reason == HvfMachine::ExitReason::Canceled,
                    "Changed watchdog deadline did not cancel the guest");
            require(elapsed >= std::chrono::milliseconds(lower) && elapsed < std::chrono::milliseconds(upper),
                    "Watchdog used a stale deadline");
        };
        // An immediate exit leaves a pending long watchdog wait. A shorter run
        // must wake it, while a longer run must survive the old earlier timeout.
        code(vm, std::array<uint32_t, 1>{0xd4000002});
        hvc(vm); // default 1000 ms budget
        timedLoop(10, 5, 500);
        code(vm, std::array<uint32_t, 1>{0xd4000002});
        auto exit = vm.run(std::chrono::milliseconds(10));
        require(exit.exceptionClass() == 0x16, "Short run did not exit normally");
        timedLoop(80, 60, 1000);
        // Expired waits while inactive must not poison the next run.
        code(vm, std::array<uint32_t, 1>{0xd4000002});
        auto immediate = vm.run(std::chrono::milliseconds(10));
        require(immediate.exceptionClass() == 0x16, "Inactive-deadline setup did not exit");
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        vm.setPC(HvfMachine::ramBase);
        hvc(vm);
    }
    {
        HvfMachine vm(2 * 1024 * 1024, true, {}, 2);
        code(vm, std::array<uint32_t, 1>{0x14000000});
        require(vm.startCPU(1, HvfMachine::ramBase, 0) == 0, "Watchdog secondary start failed");
        for (unsigned i = 0; i < 2; ++i) {
            auto start = std::chrono::steady_clock::now();
            auto exit = vm.run(std::chrono::milliseconds(20));
            require(exit.reason == HvfMachine::ExitReason::Canceled &&
                    std::chrono::steady_clock::now() - start < std::chrono::milliseconds(1000),
                    "Watchdog failed to stop both uncooperative vCPUs");
        }
    }
    {
        HvfMachine vm(2 * 1024 * 1024, true);
        code(vm, std::array<uint32_t, 14>{
            0xd2800020, 0xd518cca0, 0xd5033fdf, // SRE
            0xd2801fe0, 0xd5184600, // PMR
            0xd2800020, 0xd518cce0, 0xd5033fdf, // group 1 enabled
            0xd538cc01, 0xd4000002, // IAR -> x1, HVC
            0xd518cc21, 0xd5033fdf, 0xd538cc01, 0xd4000002}); // EOI, next IAR
        vm.nativeGicDistributorWrite(0, 0x13);
        vm.nativeGicDistributorWrite(0x88, 1); // SPI 64 group 1
        vm.nativeGicDistributorWrite(0x108, 1); // enable SPI 64
        vm.nativeGicDistributorWrite(0xc10, 2); // edge-triggered
        vm.nativeGicDistributorWrite(0x6200, 0); // route to VP0
        rejects([&] { vm.sendMSI(HvfMachine::msiSetSpi + 4, 64); }, "Wrong MSI address accepted");
        rejects([&] { vm.sendMSI(HvfMachine::msiSetSpi, 63); }, "Non-MSI SPI accepted");
        vm.sendMSI(HvfMachine::msiSetSpi, 64);
        auto exit = hvc(vm);
        require(vm.reg(1) == 64, "Native MSI was not delivered through the GIC");
        vm.setPC(exit.pc + 4);
        hvc(vm);
        require(vm.reg(1) == 1023, "MSI was delivered again after EOI");
    }
    std::cout << "{\"test\":\"self-test\",\"passed\":true,\"checks\":" << checks << "}\n";
}
}
int main(int argc, char **argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--benchmark-watchdog") {
            benchmarkWatchdog();
            return 0;
        }
        bool gui = false;
        unsigned spicePort = 0;
        std::string spicePasswordFile;
        bool test = false, timeoutExplicit = false, uefiOptions = false, cpuState = false, serialStdio = false, runForever = false;
        bool dtbExplicit = false, acpiExplicit = false;
        bool kvpProbe=false,kvpRoundtrip=false;
        auto guestPowerAction = pc::ArmHyperVShutdown::Action::PowerOff;
        pc::ArmVirtMachine::Config uefi;
        uefi.firmware = "roms/edk2-arm64.bin";
        uefi.dtb = "build/arm64-fw/pcemu-virt.dtb";
        uefi.variableTemplate = "roms/edk2-arm64-vars.bin";
        uefi.variables = "build/arm64-uefi/vars.bin";
        std::string untilText, shellCommand, framebufferPath, hypervStatsPath;
        std::string kernel, machine = "arm64-test", backend = "auto";
        unsigned timeout = 1000;
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            auto value = [&] {
                if (++i == argc)
                    throw std::runtime_error("Missing value for " + arg);
                return std::string(argv[i]);
            };
            if (arg == "--machine") machine = value();
            else if (arg == "--backend") backend = value();
            else if (arg == "--list-machines") {
                for (const auto &profile : pc::machineProfiles)
                    if (profile.runner == "pcemu-hvf")
                        std::cout << profile.id << " [available] arch=" << profile.architecture
                                  << " platform=" << profile.platform << " runner=" << profile.runner
                                  << " ram=" << profile.defaultRamMB << " MiB\n";
                return 0;
            }
            else if (arg == "--firmware") { uefi.firmware = value(); uefiOptions = true; }
            else if (arg == "--balloon-pfn64") { uefi.balloonLegacyPfn64 = true; uefi.virtioBalloon = true; uefiOptions = true; }
            else if (arg == "--virtio-gpu") { uefi.virtioGpu = true; uefiOptions = true; }
            else if (arg == "--virtio-balloon") { uefi.virtioBalloon = true; uefiOptions = true; }
            else if (arg == "--balloon-target-mb") {
                auto text = value(); size_t consumed = 0; auto mib = std::stoul(text, &consumed);
                if (consumed != text.size() || mib > 16384) throw std::runtime_error("Invalid balloon target MiB");
                uefi.balloonTargetMB = uint32_t(mib); uefi.virtioBalloon = true; uefiOptions = true;
            }
            else if (arg == "--virtio-serial") { uefi.virtioSerial = true; uefiOptions = true; }
            else if (arg == "--guest-agent-socket") { uefi.guestAgentSocket = value(); uefiOptions = true; }
            else if (arg == "--virtio-msix") { uefi.virtioMsix = true; uefiOptions = true; }
            else if (arg == "--disk") { uefi.disk = value(); uefiOptions = true; }
            else if (arg == "--extra-disk") { uefi.extraDisk = value(); uefiOptions = true; }
            else if (arg == "--extra-disk-overlay") { uefi.extraDiskOverlay = value(); uefiOptions = true; }
            else if (arg == "--ram-mb") {
                auto text = value(); size_t consumed = 0; auto mib = std::stoul(text, &consumed);
                if (consumed != text.size() || mib < 256 || mib > 16384)
                    throw std::runtime_error("ARM UEFI RAM must be 256..16384 MiB");
                uefi.memoryBytes = size_t(mib) * 1024 * 1024; uefiOptions = true;
            }
            else if (arg == "--cpus") {
                auto text = value(); size_t consumed = 0; auto count = std::stoul(text, &consumed);
                if (consumed != text.size() || count < 1 || count > 8) throw std::runtime_error("CPUs must be 1..8");
                uefi.cpus = unsigned(count); uefiOptions = true;
            }
            else if (arg == "--hyperv") {
                auto mode = value();
                if (mode != "off" && mode != "basic" && mode != "synic" && mode != "vmbus") throw std::runtime_error("Hyper-V must be off, basic, synic or vmbus");
                uefi.hyperv = mode != "off"; uefi.hypervSynic = mode == "synic" || mode == "vmbus"; uefi.hypervVmbus = mode == "vmbus"; uefiOptions = true;
            }
            else if (arg == "--hyperv-power-action") {
                auto action = value();
                if (action == "poweroff") guestPowerAction = pc::ArmHyperVShutdown::Action::PowerOff;
                else if (action == "reboot") guestPowerAction = pc::ArmHyperVShutdown::Action::Reboot;
                else throw std::runtime_error("Expected --hyperv-power-action poweroff|reboot");
                uefiOptions = true;
            }
            else if (arg == "--hyperv-kvp-roundtrip") { kvpRoundtrip=true; uefiOptions=true; }
            else if (arg == "--hyperv-kvp-probe") { kvpProbe=true; uefiOptions=true; }
            else if (arg == "--hyperv-stats") { hypervStatsPath = value(); uefiOptions = true; }
            else if (arg == "--network") {
                auto mode = value();
                if (mode != "user" && mode != "none") throw std::runtime_error("Network must be user or none");
                uefi.network = mode == "user"; uefiOptions = true;
            }
            else if (arg == "--ssh-port") {
                auto text = value(); size_t consumed = 0; auto port = std::stoul(text, &consumed);
                if (consumed != text.size() || port < 1 || port > 65535) throw std::runtime_error("SSH port must be 1..65535");
                uefi.sshPort = uint16_t(port); uefiOptions = true;
            }
            else if (arg == "--network-capture") { uefi.networkCapture = value(); uefiOptions = true; }
            else if (arg == "--serial-stdio") { serialStdio = true; uefiOptions = true; }
            else if (arg == "--spice-port") { auto text = value(); size_t consumed = 0; auto port = std::stoul(text, &consumed); if (consumed != text.size() || !port || port > 65535) throw std::runtime_error("Invalid SPICE port"); spicePort = unsigned(port); uefi.spiceAgent = true; uefiOptions = true; }
            else if (arg == "--spice-password-file") { spicePasswordFile = value(); uefiOptions = true; }
            else if (arg == "--gui") { gui = true; uefiOptions = true; }
            else if (arg == "--run-forever") { runForever = true; uefiOptions = true; }
            else if (arg == "--disk-overlay") { uefi.diskOverlay = value(); uefiOptions = true; }
            else if (arg == "--disk-controller") { uefi.diskController = value(); uefiOptions = true; }
            else if (arg == "--virtio-transport") { uefi.virtioTransport = value(); uefiOptions = true; }
            else if (arg == "--acpi-dir") { uefi.acpiDirectory = value(); acpiExplicit = true; uefiOptions = true; }
            else if (arg == "--dtb") { uefi.dtb = value(); dtbExplicit = true; uefiOptions = true; }
            else if (arg == "--vars") { uefi.variables = value(); uefiOptions = true; }
            else if (arg == "--framebuffer-ppm") { framebufferPath = value(); uefiOptions = true; }
            else if (arg == "--cpu-state") { cpuState = true; uefiOptions = true; }
            else if (arg == "--until-text") untilText = value();
            else if (arg == "--shell-command") shellCommand = value();
            else if (arg == "--self-test") test = true;
            else if (arg == "--kernel") kernel = value();
            else if (arg == "--timeout-ms") {
                auto text = value();
                size_t consumed = 0;
                auto n = std::stoul(text, &consumed);
                if (consumed != text.size() || n < 1 || n > 60000)
                    throw std::runtime_error("Timeout must be 1..60000 ms");
                timeout = unsigned(n);
                timeoutExplicit = true;
            } else if (arg == "--help") {
                std::cout << "pcemu-hvf: experimental ARM64 bare-metal platform on Apple Silicon\n"
                             "  --machine arm64-test|arm64-uefi --backend auto|hvf --list-machines\n"
                             "  --virtio-msix (requires matching --msi platform assets)\n"
                             "  --virtio-serial [--guest-agent-socket PRIVATE_PATH]\n"
                             "  --virtio-gpu (2D display and cursor)\n"
                             "  --virtio-balloon [--balloon-target-mb N] [--balloon-pfn64]\n"
                             "  --self-test\n"
                             "  --benchmark-watchdog (standalone headless RAM-only exit benchmark)\n"
                             "  --kernel FILE [--timeout-ms 1..60000]\n"
                             "Raw AArch64 code loaded at 0x40000000; EL1h, 2 MiB RAM, MMU off.\n"
                             "HVC #0 returns to host.\n"
                             "UEFI: --machine arm64-uefi [--gui | --spice-port PORT] [--spice-password-file FILE] [--firmware FILE --dtb FILE --vars PRIVATE_FILE]\n"
                             "      [--disk IMAGE [--disk-overlay PRIVATE_COW] --disk-controller virtio|nvme --virtio-transport pci|mmio]\n"
                             "      [--shell-command TEXT --until-text TEXT --timeout-ms N]\n"
                             "      [--ram-mb 256..16384 --hyperv off|basic|synic|vmbus --hyperv-stats FILE]\n"
                             "      [--hyperv-kvp-roundtrip] (private VM disposable-key mutation test)\n"
                             "      [--hyperv-kvp-probe] (one read-only intrinsic enumeration)\n"
                             "      [--hyperv-power-action poweroff|reboot] (SIGUSR2 request; default poweroff)\n"
                             "      [--framebuffer-ppm FILE --acpi-dir DIRECTORY --cpu-state]\n"
                             "      [--extra-disk IMAGE --extra-disk-overlay PRIVATE_COW]\n"
                             "      [--cpus 1..8] (build matching assets with tools/build_arm_platform.py)\n"
                             "      [--network user|none --ssh-port PORT --network-capture PCAP]\n"
                             "      [--serial-stdio --run-forever] (Ctrl-] exits serial mode)\n"
                             "Run make firmware-arm64 first; use NVMe for Windows Validation OS.\n";
                return 0;
            } else throw std::runtime_error("Unknown option: " + arg);
        }
        const auto &profile = pc::machineProfile(machine);
        if (profile.runner != "pcemu-hvf" || !profile.implemented)
            throw std::runtime_error("This runner supports only implemented ARM64 machine profiles; no OS boot yet");
        const auto selectedBackend = pc::executionBackend(backend);
        if (selectedBackend != pc::ExecutionBackend::Auto && selectedBackend != pc::ExecutionBackend::Hvf)
            throw std::runtime_error("The ARM64 test platform requires --backend hvf or auto");
        if (machine == "arm64-uefi") {
            if (uefi.cpus > 1 || uefi.memoryBytes != pc::ArmVirtMachine::ramBytes || uefi.hyperv || uefi.virtioMsix) {
                auto directory = "build/arm64-fw/cpus" + std::to_string(uefi.cpus);
                if (uefi.memoryBytes != pc::ArmVirtMachine::ramBytes)
                    directory += "-ram" + std::to_string(uefi.memoryBytes / (1024 * 1024));
                if (uefi.hyperv) directory += "-hyperv";
                if (uefi.hypervVmbus) directory += "-vmbus";
                if (uefi.virtioMsix) directory += "-msi";
                if (!dtbExplicit) uefi.dtb = directory + "/pcemu-virt.dtb";
                if (!acpiExplicit) uefi.acpiDirectory = directory + "/acpi";
            }
            if (test || !kernel.empty())
                throw std::runtime_error("The UEFI platform uses --firmware, not --kernel or --self-test");
            if (!timeoutExplicit) timeout = 30000;
            std::unique_ptr<pc::Frontend> frontend;
            if (gui && spicePort) throw std::runtime_error("Choose --gui or --spice-port");
            if (!spicePasswordFile.empty() && !spicePort) throw std::runtime_error("--spice-password-file requires --spice-port");
            pc::SpiceFrontend *spice = nullptr;
            if (spicePort) {
                auto remote = std::make_unique<pc::SpiceFrontend>(spicePort, spicePasswordFile);
                spice = remote.get(); frontend = std::move(remote);
                std::cerr << "SPICE listening on 127.0.0.1:" << spicePort << "\n";
            } else if (gui) frontend = pc::createFrontend(800, 600);
            std::vector<uint32_t> displayPixels;
            if(kvpRoundtrip && (!uefi.hypervVmbus || kvpProbe)) throw std::runtime_error("--hyperv-kvp-roundtrip requires VMBus and excludes --hyperv-kvp-probe");
            if(kvpProbe && !uefi.hypervVmbus) throw std::runtime_error("--hyperv-kvp-probe requires --hyperv vmbus");
            SerialTerminal terminal(serialStdio);
            std::signal(SIGINT, interrupt); std::signal(SIGTERM, interrupt);
            std::signal(SIGUSR1, requestCapture);
            std::signal(SIGUSR2, requestGuestShutdown);
            auto deadline = std::chrono::steady_clock::time_point::max();
            bool executionStarted = false;
            bool matched = untilText.empty(), finished = false, restart;
            bool stdinOpen = serialStdio, commandSent = shellCommand.empty();
            unsigned reboots = 0;
            do {
                auto nextInitializationPoll = std::chrono::steady_clock::now();
                auto nextIndexReport = nextInitializationPoll;
                uefi.initializationProgress = [&](uint64_t indexed, uint64_t total) {
                    if (interrupted) throw std::runtime_error("VM initialization interrupted");
                    auto now = std::chrono::steady_clock::now();
                    if (total && (now >= nextIndexReport || indexed == total)) {
                        nextIndexReport = now + std::chrono::seconds(1);
                        std::cerr << "UEFI disk open: " << indexed * 100 / total << "% ("
                                  << indexed / (1024 * 1024) << "/" << total / (1024 * 1024) << " MiB)\n";
                    }
                    if (frontend && now >= nextInitializationPoll) {
                        nextInitializationPoll = now + std::chrono::milliseconds(16);
                        for (const auto &event : frontend->poll())
                            if (event.kind == pc::FrontendEvent::Kind::Quit) interrupted = 1;
                    }
                    if (interrupted) throw std::runtime_error("VM initialization interrupted");
                };
                uefi.initializationStage = [&](const char *name) {
                    std::cerr << "UEFI initializing: " << name << "...\n";
                    uefi.initializationProgress(0, 0);
                };
                pc::ArmVirtMachine vm(uefi);
                std::cerr << "UEFI initialized: starting firmware execution\n";
                if (!executionStarted) {
                    deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
                    executionStarted = true;
                }
                pc::FrontendInput hostInput(frontend.get(), vm.guestInput());
                if (frontend) hostInput.setAbsolutePointer(true);
                struct AgentDetach {
                    pc::SpiceFrontend *spice;
                    ~AgentDetach() { if (spice) spice->pumpAgent(nullptr); }
                } agentDetach{spice};
                auto saveHypervStats = [&](const std::string &path, bool required = false) {
                    if (path.empty()) return;
                    auto temporary = path + ".tmp-" + std::to_string(getpid());
                    try {
                        std::ofstream out(temporary);
                        out << vm.hypervReport(); out.close();
                        if (!out) throw std::runtime_error("Cannot write Hyper-V statistics: " + path);
                        std::filesystem::rename(temporary, path);
                    } catch (const std::exception &error) {
                        std::error_code ignored;
                        std::filesystem::remove(temporary, ignored);
                        if (required) throw;
                        std::cerr << "Hyper-V statistics unavailable: " << error.what() << '\n';
                    }
                };
                saveHypervStats(hypervStatsPath, true);
                size_t printed = 0, matchStart = 0;
                bool kvpProbeQueued=false;
                std::optional<pc::HyperVKvpRoundtrip> kvpRoundtripState;
                bool kvpRoundtripReported=false;
                if(kvpRoundtrip) {
                    auto key="PCEmuValidation-"+std::to_string(getpid())+"-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
                    kvpRoundtripState.emplace(std::u16string(key.begin(),key.end()));
                    std::cerr<<"Hyper-V KVP disposable key: "<<key<<'\n';
                }
                auto nextFrame = std::chrono::steady_clock::now();
                while (!vm.stopped() && !interrupted && (runForever || std::chrono::steady_clock::now() < deadline)) {
                    if (stdinOpen) {
                        pollfd input{STDIN_FILENO, POLLIN, 0};
                        if (poll(&input, 1, 0) > 0) {
                            std::array<char, 1024> bytes{};
                            auto count = read(STDIN_FILENO, bytes.data(), bytes.size());
                            if (count > 0) {
                                std::string text(bytes.data(), size_t(count));
                                auto escape = text.find(char(0x1d));
                                if (escape != std::string::npos) { text.resize(escape); interrupted = 1; }
                                vm.input(text);
                            } else stdinOpen = false;
                        }
                    }
                    // A run call can return after one MMIO instruction. Rendering
                    // on every exit throttles firmware to the host display rate.
                    auto now = std::chrono::steady_clock::now();
                    bool frameDue = frontend && now >= nextFrame;
                    bool redraw = false;
                    if (frameDue) {
                        nextFrame = now + std::chrono::milliseconds(16);
                        for (const auto &event : frontend->poll()) {
                            if (event.kind == pc::FrontendEvent::Kind::Redraw) redraw = true;
                            if (!hostInput.event(event)) interrupted = 1;
                        }
                    }
                    if (interrupted) break;
                    if (spice) spice->pumpAgent(vm.spiceSerial());
                    vm.run(std::chrono::milliseconds(frontend ? 16 : 100));
                    if (frameDue && !interrupted) {
                        unsigned width = 0, height = 0;
                        bool changed = vm.display(displayPixels, width, height, false);
                        frontend->setPointerShape(vm.pointerShape());
                        if (width && height && (changed || redraw))
                            frontend->present(displayPixels, width, height);
                    }
                    if(kvpRoundtripState && !kvpRoundtripReported) {
                        kvpRoundtripState->tick(vm);
                        if(kvpRoundtripState->finished()) {
                            std::cerr<<"Hyper-V KVP roundtrip: "<<kvpRoundtripState->report()<<'\n';
                            kvpRoundtripReported=true;
                        }
                    }
                    if(kvpProbe) {
                        if(!kvpProbeQueued) kvpProbeQueued=vm.enumerateGuestKvp(pc::ArmHyperVKvpCodec::Pool::Auto,0);
                        if(auto result=vm.takeGuestKvpResult()) {
                            auto encoded=[](const std::u16string &text) {
                                std::string out;constexpr char hex[]="0123456789abcdef";
                                for(auto c:text){out+="\\u";for(int shift=12;shift>=0;shift-=4)out+=hex[(c>>shift)&15];}
                                return out;
                            };
                            std::cerr<<"Hyper-V KVP probe: index="<<result->index<<" status="<<result->status;
                            if(result->record)std::cerr<<" key="<<encoded(result->record->key)<<" value="<<encoded(result->record->value.text);
                            std::cerr<<'\n';
                        }
                    }
                    if (guestShutdownRequested) {
                        guestShutdownRequested = 0;
                        std::cerr << (vm.requestGuestShutdown(guestPowerAction)
                            ? (guestPowerAction == pc::ArmHyperVShutdown::Action::Reboot
                                ? "Hyper-V guest reboot requested\n" : "Hyper-V guest shutdown requested\n")
                            : "Hyper-V guest shutdown unavailable (channel not ready)\n");
                    }
                    if (captureRequested) {
                        captureRequested = 0;
                        saveHypervStats(hypervStatsPath);
                        if (cpuState) std::cerr << "ARM live CPU: " << vm.cpuState() << '\n';
                        if (!framebufferPath.empty()) {
                            try { vm.saveFramebuffer(framebufferPath); }
                            catch (const std::exception &error) {
                                std::cerr << "UEFI live capture unavailable: " << error.what() << '\n';
                            }
                        }
                        std::cerr << "UEFI live capture: exits=" << vm.exits()
                                  << " ramMiB=" << uefi.memoryBytes / (1024 * 1024)
                                  << " diskRequests=" << vm.diskRequests()
                                  << " netTx=" << vm.networkTransmitted()
                                  << " netRx=" << vm.networkReceived()
                                  << " framebuffer=" << vm.framebuffer() << " usb=" << vm.usbReport() << '\n';
                    }
                    const auto &output = vm.output();
                    if (output.size() > printed) {
                        std::cout.write(output.data() + printed, output.size() - printed);
                        printed = output.size();
                        std::cout.flush();
                    }
                    if (!commandSent && output.find("Shell> ") != std::string::npos) {
                        vm.input(shellCommand + "\r"); commandSent = true; matchStart = output.size();
                    }
                    if (!untilText.empty() && output.find(untilText, matchStart) != std::string::npos) {
                        matched = true; finished = true; break;
                    }
                }
                if (interrupted) std::cerr << "UEFI stopping: saving variables and flushing disks...\n";
                hostInput.release();
                saveHypervStats(hypervStatsPath);
                if (!hypervStatsPath.empty()) saveHypervStats(hypervStatsPath + ".boot" + std::to_string(reboots) + ".json");
                if(kvpRoundtripState && !kvpRoundtripReported)
                    std::cerr<<"Hyper-V KVP roundtrip: "<<kvpRoundtripState->report()<<'\n';
                vm.saveVariables();
                if (cpuState) std::cerr << "ARM CPU: " << vm.cpuState() << '\n';
                if (!framebufferPath.empty()) vm.saveFramebuffer(framebufferPath);
                std::cerr << "UEFI run complete: exits=" << vm.exits() << " cpus=" << uefi.cpus << " variables=" << uefi.variables
                          << " ramMiB=" << uefi.memoryBytes / (1024 * 1024) << " disk=" << vm.diskPath() << " virtioRequests=" << vm.diskRequests()
                          << " extraDiskRequests=" << vm.extraDiskRequests()
                          << " netTx=" << vm.networkTransmitted() << " netRx=" << vm.networkReceived()
                          << " framebuffer=" << vm.framebuffer() << " usb=" << vm.usbReport() << " state=" << vm.stopReason() << '\n';
                restart = vm.resetRequested() && !interrupted && !finished &&
                    (runForever || std::chrono::steady_clock::now() < deadline);
                if (restart) std::cerr << "UEFI restarting after guest reset: reboot=" << ++reboots << '\n';
                // Destroy devices, sockets and HVF before recreating the platform.
                // Saved variables and flushed disks are reopened by the next boot.
            } while (restart);
            return matched ? 0 : 1;
        }
        if (uefiOptions || !untilText.empty() || !shellCommand.empty())
            throw std::runtime_error("UEFI options require --machine arm64-uefi");
        if (test && !kernel.empty())
            throw std::runtime_error("Choose --self-test or --kernel");
        if (test) {
            selfTest();
            return 0;
        }
        if (kernel.empty())
            throw std::runtime_error("Specify --self-test or --kernel FILE; see --help");
        std::ifstream input(kernel, std::ios::binary | std::ios::ate);
        if (!input)
            throw std::runtime_error("Cannot open guest binary: " + kernel);
        auto length = input.tellg();
        if (length <= 0 || length > 2 * 1024 * 1024 || length % 4)
            throw std::runtime_error("Guest binary must be 4-byte aligned and fit in 2 MiB RAM");
        input.seekg(0);
        std::vector<uint8_t> bytes(static_cast<size_t>(length));
        if (!input.read(reinterpret_cast<char *>(bytes.data()), length))
            throw std::runtime_error("Cannot read guest binary");
        HvfMachine vm;
        vm.writeMemory(HvfMachine::ramBase, bytes);
        vm.start(HvfMachine::ramBase);
        auto exit = vm.run(std::chrono::milliseconds(timeout));
        std::cout << "exit=" << unsigned(exit.reason) << " ec=0x" << std::hex
                  << exit.exceptionClass() << " syndrome=0x" << exit.syndrome
                  << " pc=0x" << exit.pc << " ipa=0x" << exit.physicalAddress
                  << " x0=0x" << vm.reg(0) << std::dec << " exits=" << vm.exits() << '\n';
        return exit.reason == HvfMachine::ExitReason::Exception &&
                       exit.exceptionClass() == 0x16 && (exit.syndrome & 0xffff) == 0 ? 0 : 1;
    } catch (const std::exception &e) {
        std::cerr << "pcemu-hvf: " << e.what() << '\n';
        return 1;
    }
}
