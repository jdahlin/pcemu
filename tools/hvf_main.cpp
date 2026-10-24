#include "hvf_machine.hpp"
#include "arm_virt.hpp"
#include "machine_profile.hpp"
#include <array>
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
using pc::HvfMachine;
namespace {
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
    std::cout << "{\"test\":\"self-test\",\"passed\":true,\"checks\":" << checks << "}\n";
}
}
int main(int argc, char **argv) {
    try {
        bool test = false, timeoutExplicit = false, uefiOptions = false, cpuState = false;
        pc::ArmVirtMachine::Config uefi{"roms/edk2-arm64.bin", "build/arm64-fw/pcemu-virt.dtb",
                                        "roms/edk2-arm64-vars.bin", "build/arm64-uefi/vars.bin", {}, {}, "pci"};
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
            else if (arg == "--disk-overlay") { uefi.diskOverlay = value(); uefiOptions = true; }
            else if (arg == "--virtio-transport") { uefi.virtioTransport = value(); uefiOptions = true; }
            else if (arg == "--acpi-dir") { uefi.acpiDirectory = value(); uefiOptions = true; }
            else if (arg == "--dtb") { uefi.dtb = value(); uefiOptions = true; }
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
                             "      [--disk IMAGE [--disk-overlay PRIVATE_COW] --virtio-transport pci|mmio]\n"
                             "      [--shell-command TEXT --until-text TEXT --timeout-ms N]\n"
                             "      [--framebuffer-ppm FILE --acpi-dir DIRECTORY --cpu-state]\n"
                             "Run make firmware-arm64 first; OS boot is not implemented yet.\n";
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
            if (test || !kernel.empty())
                throw std::runtime_error("The UEFI platform uses --firmware, not --kernel or --self-test");
            if (!timeoutExplicit) timeout = 30000;
            pc::ArmVirtMachine vm(uefi);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
            size_t printed = 0, matchStart = 0;
            bool commandSent = shellCommand.empty(), matched = untilText.empty();
            while (!vm.stopped() && std::chrono::steady_clock::now() < deadline) {
                vm.run(std::chrono::milliseconds(100));
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
                    matched = true; break;
                }
            }
            vm.saveVariables();
            if (cpuState) std::cerr << "ARM CPU: " << vm.cpuState() << '\n';
            if (!framebufferPath.empty()) vm.saveFramebuffer(framebufferPath);
            std::cerr << "UEFI run complete: exits=" << vm.exits() << " variables=" << uefi.variables
                      << " disk=" << vm.diskPath() << " virtioRequests=" << vm.diskRequests()
                      << " framebuffer=" << vm.framebuffer() << " state=" << vm.stopReason() << '\n';
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
