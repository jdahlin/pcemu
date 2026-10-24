#include "hvf_machine.hpp"
#include "arm_virt.hpp"
#include "machine_profile.hpp"
#include <array>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <csignal>
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#include <mach/mach_time.h>
using pc::HvfMachine;
namespace {
volatile std::sig_atomic_t interrupted = 0;
void interrupt(int) { interrupted = 1; }
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
HvfMachine::Exit hvc(HvfMachine &vm) {
    auto exit = vm.run();
    // A watchdog request racing a completed exit may leave one pending cancel.
    for (unsigned retry = 0; retry < 2 && exit.reason == HvfMachine::ExitReason::Canceled; ++retry)
        exit = vm.run();
    require(exit.reason == HvfMachine::ExitReason::Exception && exit.exceptionClass() == 0x16,
            "Expected an AArch64 HVC exit");
    return exit;
}
void selfTest() {
    // Invalid configuration must fail before claiming the process-wide VM.
    rejects([] { HvfMachine invalid(123); }, "Unaligned memory accepted");
    {
        HvfMachine vm(size_t(8) * 1024 * 1024 * 1024);
        code(vm, std::array<uint32_t, 3>{0xb9000020, 0xb9400022, 0xd4000002});
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
    std::cout << "{\"test\":\"self-test\",\"passed\":true,\"checks\":" << checks << "}\n";
}
}
int main(int argc, char **argv) {
    try {
        bool test = false, timeoutExplicit = false, uefiOptions = false, cpuState = false, serialStdio = false, runForever = false;
        bool dtbExplicit = false, acpiExplicit = false;
        pc::ArmVirtMachine::Config uefi;
        uefi.firmware = "roms/edk2-arm64.bin";
        uefi.dtb = "build/arm64-fw/pcemu-virt.dtb";
        uefi.variableTemplate = "roms/edk2-arm64-vars.bin";
        uefi.variables = "build/arm64-uefi/vars.bin";
        std::string untilText, shellCommand, framebufferPath;
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
                             "  --self-test\n"
                             "  --kernel FILE [--timeout-ms 1..60000]\n"
                             "Raw AArch64 code loaded at 0x40000000; EL1h, 2 MiB RAM, MMU off.\n"
                             "HVC #0 returns to host.\n"
                             "UEFI: --machine arm64-uefi [--firmware FILE --dtb FILE --vars PRIVATE_FILE]\n"
                             "      [--disk IMAGE [--disk-overlay PRIVATE_COW] --disk-controller virtio|nvme --virtio-transport pci|mmio]\n"
                             "      [--shell-command TEXT --until-text TEXT --timeout-ms N]\n"
                             "      [--ram-mb 256..16384]\n"
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
            if (uefi.cpus > 1 || uefi.memoryBytes != pc::ArmVirtMachine::ramBytes) {
                auto directory = "build/arm64-fw/cpus" + std::to_string(uefi.cpus);
                if (uefi.memoryBytes != pc::ArmVirtMachine::ramBytes)
                    directory += "-ram" + std::to_string(uefi.memoryBytes / (1024 * 1024));
                if (!dtbExplicit) uefi.dtb = directory + "/pcemu-virt.dtb";
                if (!acpiExplicit) uefi.acpiDirectory = directory + "/acpi";
            }
            if (test || !kernel.empty())
                throw std::runtime_error("The UEFI platform uses --firmware, not --kernel or --self-test");
            if (!timeoutExplicit) timeout = 30000;
            SerialTerminal terminal(serialStdio);
            std::signal(SIGINT, interrupt); std::signal(SIGTERM, interrupt);
            std::signal(SIGUSR1, requestCapture);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
            bool matched = untilText.empty(), finished = false, restart;
            bool stdinOpen = serialStdio, commandSent = shellCommand.empty();
            unsigned reboots = 0;
            do {
                pc::ArmVirtMachine vm(uefi);
                size_t printed = 0, matchStart = 0;
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
                    vm.run(std::chrono::milliseconds(100));
                    if (captureRequested) {
                        captureRequested = 0;
                        if (!framebufferPath.empty()) vm.saveFramebuffer(framebufferPath);
                        std::cerr << "UEFI live capture: exits=" << vm.exits()
                                  << " ramMiB=" << uefi.memoryBytes / (1024 * 1024)
                                  << " diskRequests=" << vm.diskRequests()
                                  << " netTx=" << vm.networkTransmitted()
                                  << " netRx=" << vm.networkReceived()
                                  << " framebuffer=" << vm.framebuffer() << '\n';
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
                vm.saveVariables();
                if (cpuState) std::cerr << "ARM CPU: " << vm.cpuState() << '\n';
                if (!framebufferPath.empty()) vm.saveFramebuffer(framebufferPath);
                std::cerr << "UEFI run complete: exits=" << vm.exits() << " cpus=" << uefi.cpus << " variables=" << uefi.variables
                          << " ramMiB=" << uefi.memoryBytes / (1024 * 1024) << " disk=" << vm.diskPath() << " virtioRequests=" << vm.diskRequests()
                          << " extraDiskRequests=" << vm.extraDiskRequests()
                          << " netTx=" << vm.networkTransmitted() << " netRx=" << vm.networkReceived()
                          << " framebuffer=" << vm.framebuffer() << " state=" << vm.stopReason() << '\n';
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
