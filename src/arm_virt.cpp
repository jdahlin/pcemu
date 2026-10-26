#include "arm_virt.hpp"
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <cstdlib>
#include <sstream>
#include <stdexcept>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
namespace pc {
namespace fs = std::filesystem;
namespace {
void validateCpuTables(std::span<const uint8_t> blob, unsigned expected, bool hyperv, bool vmbus, bool msi) {
    bool foundFadt = false, foundVmbus = false;
    for (size_t at = 0; at + 36 <= blob.size();) {
        uint32_t size = 0;
        for (unsigned i = 0; i < 4; ++i) size |= uint32_t(blob[at + 4 + i]) << (8 * i);
        if (size < 36 || size > blob.size() - at) throw std::runtime_error("Invalid ACPI table length");
        if (std::string(reinterpret_cast<const char *>(blob.data() + at), 4) == "FACP") {
            if (size < 276) throw std::runtime_error("FADT lacks hypervisor identity field");
            bool advertised = std::string(reinterpret_cast<const char *>(blob.data() + at + 268), 8) == "MsHyperV";
            if (advertised != hyperv) throw std::runtime_error("ACPI Hyper-V identity does not match --hyperv; rebuild matching platform assets");
            foundFadt = true;
        }
        if (std::string(reinterpret_cast<const char *>(blob.data() + at), 4) == "SSDT" &&
            std::string(reinterpret_cast<const char *>(blob.data() + at + 16), 8) == "VMBUS   ") {
            if (foundVmbus) throw std::runtime_error("Duplicate VMBus SSDT");
            foundVmbus = true;
        }
        at = (at + size + 7) & ~size_t(7);
    }
    if (foundVmbus != vmbus) throw std::runtime_error("ACPI VMBus does not match --hyperv; rebuild matching platform assets");
    if (!foundFadt) throw std::runtime_error("Missing FADT");
    auto little = [&](size_t at, unsigned width) {
        if (at > blob.size() || width > blob.size() - at) throw std::runtime_error("Truncated ACPI CPU table");
        uint64_t value = 0;
        for (unsigned i = 0; i < width; ++i) value |= uint64_t(blob[at + i]) << (8 * i);
        return value;
    };
    for (size_t at = 0; at + 36 <= blob.size();) {
        auto length = size_t(little(at + 4, 4));
        if (length < 36 || length > blob.size() - at) throw std::runtime_error("Invalid ACPI table length");
        if (std::string(reinterpret_cast<const char *>(blob.data() + at), 4) == "APIC") {
            unsigned cpus = 0;
            bool msiFrame = false;
            for (size_t entry = at + 44; entry + 2 <= at + length;) {
                auto bytes = blob[entry + 1];
                if (bytes < 2 || bytes > at + length - entry) throw std::runtime_error("Invalid MADT entry");
                if (blob[entry] == 11 && bytes >= 80 && (little(entry + 12, 4) & 1)) {
                    if (little(entry + 8, 4) != cpus || little(entry + 68, 8) != cpus)
                        throw std::runtime_error("ACPI CPU affinity does not match the HVF topology");
                    ++cpus;
                }
                if (blob[entry] == 13 && bytes == 24 && little(entry + 8, 8) == HvfMachine::msiFrameBase &&
                    little(entry + 16, 4) == 1 && little(entry + 20, 2) == HvfMachine::msiCount &&
                    little(entry + 22, 2) == HvfMachine::msiBase) msiFrame = true;
                entry += bytes;
            }
            if (msi && !msiFrame) throw std::runtime_error("MSI-X requires ARM platform assets built with --msi");
            if (cpus != expected) throw std::runtime_error("ACPI CPU count does not match --cpus; build matching ARM platform assets");
            return;
        }
        at = (at + length + 7) & ~size_t(7);
    }
    throw std::runtime_error("Missing ACPI CPU table");
}
}
std::vector<uint8_t> ArmVirtMachine::load(const std::string &path, size_t maximum) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    auto length = input.tellg();
    if (!input || length <= 0 || uint64_t(length) > maximum)
        throw std::runtime_error("Cannot read ARM platform asset: " + path);
    std::vector<uint8_t> data(static_cast<size_t>(length));
    input.seekg(0);
    if (!input.read(reinterpret_cast<char *>(data.data()), length))
        throw std::runtime_error("Short ARM platform asset: " + path);
    return data;
}
ArmVirtMachine::ArmVirtMachine(const Config &c)
    : config(c), vm(c.memoryBytes, c.hypervSynic ? HvfMachine::GicProfile::HyperVNonsecure : HvfMachine::GicProfile::Native, {}, c.cpus, c.initializationStage), nor(load(c.variableTemplate, 0xc0000)),
      fwCfg(vm.ram(), HvfMachine::ramBase, c.cpus), ramfb(fwCfg, vm.ram(), HvfMachine::ramBase), osLocks(c.cpus),
      hyperv(c.hyperv, vm.ram(), HvfMachine::ramBase, c.cpus) {
    auto stage = [&](const char *name) { if (c.initializationStage) c.initializationStage(name); };
    stage("loading firmware platform tables");
    if(c.hyperv) {
        hyperv.enableReferenceClock([this] {
            auto sample=vm.virtualCounterSample();
            return ArmHyperV::CounterSample{sample.value,sample.frequency};
        });
        vm.setCounterRebaseHandler([this](bool before) {hyperv.rebaseReferenceClock(before);});
    }
    hyperv.setPrivateInterruptSender([this](uint64_t targets, unsigned interrupt) {
        vm.sendPrivateInterrupt(targets, interrupt);
        return true;
    });
    if(c.hypervSynic) gicView=std::make_unique<ArmGicNonsecureView>(
        [this](uint32_t offset) {return vm.nativeGicDistributorRead(offset);},
        [this](uint32_t offset,uint64_t value) {vm.nativeGicDistributorWrite(offset,value);});
    hyperv.advertiseSynic(c.hypervSynic);
    if (c.hypervVmbus) { hyperv.enableVmbus(); hyperv.enableHeartbeat(); hyperv.enableTimeSync(); hyperv.enableShutdown(); hyperv.enableKvp(); }
    hyperv.setSoftwareInterruptSender([this](uint64_t targets, unsigned interrupt) {
        vm.sendSoftwareInterrupt(targets, interrupt);
        return true;
    });
    hyperv.setContextSynchronizer([this](uint64_t targets) {
        vm.synchronizeInstructionContext(targets);
        return true;
    });
    hyperv.setTranslationFlusher([this](uint64_t targets) {
        vm.flushTranslationContext(targets);
        return true;
    });
    fwCfg.addFile("etc/ramfb", std::vector<uint8_t>(28), true);
    auto tables = load(c.acpiDirectory + "/tables", 64 * 1024);
    validateCpuTables(tables, c.cpus, c.hyperv, c.hypervVmbus, c.virtioMsix);
    fwCfg.addFile("etc/acpi/tables", std::move(tables));
    fwCfg.addFile("etc/acpi/rsdp", load(c.acpiDirectory + "/rsdp", 36));
    fwCfg.addFile("etc/table-loader", load(c.acpiDirectory + "/table-loader", 64 * 1024));
    const auto working = fs::absolute(c.variables).lexically_normal();
    config.variables = working.string();
    const auto original = fs::absolute(c.variableTemplate).lexically_normal();
    if (working == original || (fs::exists(working) && fs::equivalent(working, original)))
        throw std::runtime_error("UEFI variables must use a private file, not the firmware template");
    fs::create_directories(working.parent_path());
    stage("opening UEFI variable store");
    variableLock = open((working.string() + ".lock").c_str(), O_CREAT | O_RDWR, 0600);
    if (variableLock < 0 || flock(variableLock, LOCK_EX | LOCK_NB) != 0) {
        if (variableLock >= 0) close(variableLock);
    if (diskLock >= 0) close(diskLock);
        variableLock = -1;
        throw std::runtime_error("UEFI variable file is already in use or cannot be locked");
    }
    try {
        if (fs::exists(working)) {
            auto bytes = load(working.string(), 0xc0000);
            if (bytes.size() != nor.bytes().size())
                throw std::runtime_error("UEFI variable file has wrong size");
            nor = ArmNorFlash(bytes);
        }
        if (c.virtioTransport != "pci" && c.virtioTransport != "mmio")
            throw std::runtime_error("Virtio transport must be pci or mmio");
        if (c.diskController != "virtio" && c.diskController != "nvme")
            throw std::runtime_error("Disk controller must be virtio or nvme");
        if (c.diskController == "nvme" && c.virtioTransport != "pci")
            throw std::runtime_error("NVMe requires PCI transport");
        VirtioPciDevice::Message msi;
        if (c.virtioMsix) msi = [this](uint64_t address, uint32_t data) { vm.sendMSI(address, data); };
        if (!c.disk.empty()) {
            const auto parent = fs::canonical(c.disk).string();
            std::ifstream probe(parent, std::ios::binary);
            std::array<char, 8> signature{};
            probe.read(signature.data(), signature.size());
            bool direct = c.diskOverlay.empty() && std::string(signature.data(), signature.size()) == "vhdxfile";
            auto overlay = direct ? parent : c.diskOverlay.empty() ? config.variables + ".qcow2" : fs::absolute(c.diskOverlay).string();
            fs::create_directories(fs::path(overlay).parent_path());
            if (!direct && fs::exists(overlay) && fs::equivalent(parent, overlay))
                throw std::runtime_error("Virtio disk overlay must differ from immutable parent");
            diskLock = open((overlay + ".lock").c_str(), O_CREAT | O_RDWR, 0600);
            if (diskLock < 0 || flock(diskLock, LOCK_EX | LOCK_NB) != 0)
                throw std::runtime_error("Virtio disk is already in use or cannot be locked");
            if (!direct && !fs::exists(overlay)) Disk::createOverlay(overlay, parent);
            stage("opening primary disk");
            disk = std::make_unique<Disk>(overlay, false, false, 0, c.initializationProgress);
            if (!direct && (!disk->isOverlay() || fs::canonical(disk->backingPath()).string() != parent))
                throw std::runtime_error("Virtio disk overlay belongs to a different parent");
            if (c.diskController == "nvme") {
                nvme = std::make_unique<Nvme>(*disk, vm.ram(), HvfMachine::ramBase,
                                            [this](bool level) { vm.setSPI(48, level); });
            } else {
                block = std::make_unique<VirtioBlock>(*disk, vm.ram(), HvfMachine::ramBase,
                                                     [this](bool level) { if (pci) pci->irq(level); else vm.setSPI(48, level); });
                if (c.virtioTransport == "pci")
                    pci = std::make_unique<VirtioPciBlock>(*block, [this](bool level) { vm.setSPI(48, level); }, msi);
            }
        } else if (!c.diskOverlay.empty()) throw std::runtime_error("--disk-overlay requires --disk");
        if (!c.extraDisk.empty()) {
            auto parent = fs::canonical(c.extraDisk).string();
            auto overlay = c.extraDiskOverlay.empty() ? config.variables + ".extra.qcow2" : fs::absolute(c.extraDiskOverlay).string();
            fs::create_directories(fs::path(overlay).parent_path());
            if (fs::exists(overlay) && fs::equivalent(parent, overlay))
                throw std::runtime_error("Extra disk overlay must differ from immutable parent");
            extraDiskLock = open((overlay + ".lock").c_str(), O_CREAT | O_RDWR, 0600);
            if (extraDiskLock < 0 || flock(extraDiskLock, LOCK_EX | LOCK_NB))
                throw std::runtime_error("Extra disk is already in use or cannot be locked");
            if (!fs::exists(overlay)) Disk::createOverlay(overlay, parent);
            stage("opening extra disk");
            extraDisk = std::make_unique<Disk>(overlay, false, false, 0, c.initializationProgress);
            if (!extraDisk->isOverlay() || fs::canonical(extraDisk->backingPath()).string() != parent)
                throw std::runtime_error("Extra disk overlay belongs to a different parent");
            extraBlock = std::make_unique<VirtioBlock>(*extraDisk, vm.ram(), HvfMachine::ramBase,
                [this](bool level) { if (extraPci) extraPci->irq(level); });
            extraPci = std::make_unique<VirtioPciDevice>(*extraBlock, [this](bool level) { vm.setSPI(49, level); }, msi);
        } else if (!c.extraDiskOverlay.empty()) throw std::runtime_error("--extra-disk-overlay requires --extra-disk");
        stage("creating PCI devices and network");
        if (c.network) {
            net = std::make_unique<VirtioNet>(vm.ram(), HvfMachine::ramBase,
                [this](bool level) { if (netPci) netPci->irq(level, net->interruptQueue()); });
            netPci = std::make_unique<VirtioPciDevice>(
                [this](unsigned o, unsigned w) { return net->read(o, w); },
                [this](unsigned o, uint64_t v, unsigned w) { net->write(o, v, w); },
                1, 0x02000001, 2, [this](bool level) { vm.setSPI(50, level); }, msi);
            gateway = std::make_unique<Network>(c.networkCapture);
            gateway->enableWakeups([this] { vm.requestExit(); });
            if (c.sshPort) gateway->forwardTCP(c.sshPort, 22);
        } else if (c.sshPort || !c.networkCapture.empty()) throw std::runtime_error("SSH forwarding/capture requires --network user");
        usb = std::make_unique<UsbXhci>(vm.ram(), HvfMachine::ramBase,
            [this](bool level) { vm.setSPI(52, level); });
        usbInput = std::make_unique<UsbGuestInput>(*usb);
        rng = std::make_unique<VirtioRng>(vm.ram(), HvfMachine::ramBase,
            [this](bool level) { if (rngPci) rngPci->irq(level); });
        rngPci = std::make_unique<VirtioPciDevice>(
            [this](unsigned o, unsigned w) { return rng->read(o, w); },
            [this](unsigned o, uint64_t v, unsigned w) { rng->write(o, v, w); },
            4, 0x00ff0001, 1, [this](bool level) { vm.setSPI(51, level); }, msi);
        if (c.virtioSerial || !c.guestAgentSocket.empty()) {
            serial = std::make_unique<VirtioSerial>(vm.ram(), HvfMachine::ramBase,
                [this](bool level, unsigned queue) { if (serialPci) serialPci->irq(level, queue); });
            serialPci = std::make_unique<VirtioPciDevice>(
                [this](unsigned o, unsigned w) { return serial->read(o, w); },
                [this](unsigned o, uint64_t v, unsigned w) { serial->write(o, v, w); },
                3, 0x07800001, 4, [this](bool level) { vm.setSPI(53, level); }, msi);
            if (!c.guestAgentSocket.empty()) agentSocket = std::make_unique<GuestAgentSocket>(c.guestAgentSocket);
        }
        bindDevices();
        stage("loading and mapping firmware ROM");
        auto firmware = load(c.firmware, 16 * 1024 * 1024);
        auto dtb = load(c.dtb, 64 * 1024);
        if (dtb.size() < 40 || dtb[0] != 0xd0 || dtb[1] != 0x0d || dtb[2] != 0xfe || dtb[3] != 0xed)
            throw std::runtime_error("Invalid ARM device tree");
        vm.mapROM(0, firmware);
        vm.mapROM(0x04000000, nor.bytes());
        vm.writeMemory(HvfMachine::ramBase, dtb);
        vm.start(0);
        vm.setReg(0, HvfMachine::ramBase);
    } catch (...) {
        close(variableLock); variableLock = -1;
        if (diskLock >= 0) { close(diskLock); diskLock = -1; }
        if (extraDiskLock >= 0) { close(extraDiskLock); extraDiskLock = -1; }
        throw;
    }
}
ArmVirtMachine::~ArmVirtMachine() {
    vm.setCounterRebaseHandler({});
    if (variableLock >= 0) close(variableLock);
    if (diskLock >= 0) close(diskLock);
    if (extraDiskLock >= 0) close(extraDiskLock);
}
void ArmVirtMachine::saveVariables() {
    if (disk) disk->flush();
    if (extraDisk) extraDisk->flush();
    auto target = fs::path(config.variables);
    auto temporary = fs::path(target.string() + ".tmp-" + std::to_string(getpid()));
    try {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char *>(nor.storage().data()), nor.storage().size());
        output.close();
        if (!output) throw std::runtime_error("Cannot save UEFI variables");
        fs::rename(temporary, target);
    } catch (...) { fs::remove(temporary); throw; }
}
std::string ArmVirtMachine::cpuState() const {
    auto elr = vm.systemReg(0xc201);
    std::ostringstream output;
    output << std::hex << "pc=0x" << vm.pc() << " esr_el1=0x" << vm.systemReg(0xc290)
           << " elr_el1=0x" << elr << " far_el1=0x" << vm.systemReg(0xc300)
           << " spsr_el1=0x" << vm.systemReg(0xc200);
    for (unsigned i = 0; i < vm.cpuCount(); ++i) output << " cpu" << i << "_pc=0x" << vm.cpuPC(i);
    auto dump = [&](const char *name, uint64_t address) {
        if (address < HvfMachine::ramBase + 16 || address - HvfMachine::ramBase > config.memoryBytes - 48) return;
        std::array<uint8_t, 64> bytes{};
        vm.readMemory(address - 16, bytes);
        output << ' ' << name << "_start=0x" << address - 16 << ' ' << name << "_bytes=";
        for (auto byte : bytes) output << std::setw(2) << std::setfill('0') << unsigned(byte);
    };
    dump("elr", elr);
    const std::string marker = "Synchronous Exception at 0x";
    auto at = uart.output().find(marker);
    if (at != std::string::npos) {
        std::istringstream input(uart.output().substr(at + marker.size(), 16));
        uint64_t address = 0;
        if (input >> std::hex >> address) dump("reported", address);
    }
    if (nvme) output << ' ' << nvme->diagnostics();
    return output.str();
}
std::string ArmVirtMachine::hypervReport() const {
    auto report = hyperv.report(); report.resize(report.find_last_of('}'));
    if (serial) report += ",\"virtioSerial\":{\"ready\":" + std::string(serial->portReady() ? "true" : "false") +
        ",\"open\":" + std::string(serial->portOpen() ? "true" : "false") +
        ",\"received\":" + std::to_string(serial->received) + ",\"transmitted\":" + std::to_string(serial->transmitted) +
        ",\"completions\":" + std::to_string(serial->completions) + ",\"messageInterrupts\":" + std::to_string(serialPci->messageInterrupts) + "}";
    report += ",\"instructionContextBarriers\":[";
    for (unsigned cpu = 0; cpu < vm.cpuCount(); ++cpu) {
        if (cpu) report += ',';
        report += std::to_string(vm.instructionContextBarriers(cpu));
    }
    report += "],\"translationContextFlushes\":[";
    for (unsigned cpu = 0; cpu < vm.cpuCount(); ++cpu) {
        if (cpu) report += ',';
        report += std::to_string(vm.translationContextFlushes(cpu));
    }
    report += "],\"softwareInterruptRequests\":[";
    for (unsigned cpu = 0; cpu < vm.cpuCount(); ++cpu) {
        if (cpu) report += ',';
        report += std::to_string(vm.softwareInterruptRequests(cpu));
    }
    report += "],\"privateInterruptRequests\":[";
    for (unsigned cpu = 0; cpu < vm.cpuCount(); ++cpu) {
        if (cpu) report += ',';
        report += std::to_string(vm.privateInterruptRequests(cpu));
    }
    report += "],\"privateInterruptState\":[";
    for (unsigned cpu = 0; cpu < vm.cpuCount(); ++cpu) {
        if (cpu) report += ',';
        const auto gic = vm.privateInterruptState(cpu);
        report += "{\"available\":" + std::string(gic.available ? "true" : "false") +
                  ",\"enabled\":" + std::to_string(gic.enabled) +
                  ",\"pending\":" + std::to_string(gic.pending) +
                  ",\"active\":" + std::to_string(gic.active) + '}';
    }
    report += "],\"virtioMsix\":{";
    bool comma = false;
    for (const auto &[name, device] : std::initializer_list<std::pair<const char *, VirtioPciDevice *>>{
            {"block", pci.get()}, {"extraBlock", extraPci.get()}, {"network", netPci.get()}, {"rng", rngPci.get()}}) {
        if (!device) continue;
        if (comma) report += ',';
        comma = true;
        report += "\"" + std::string(name) + "\":{\"enabled\":" +
            (device->msixEnabled() ? "true" : "false") + ",\"messages\":" + std::to_string(device->messageInterrupts) + '}';
    }
    return report + "}}";
}
void ArmVirtMachine::updateRtcInterrupt() {
    bool level = rtc.irq();
    if (level == rtcInterruptLevel) return;
    vm.setSPI(34, level);
    rtcInterruptLevel = level;
}
void ArmVirtMachine::run(std::chrono::milliseconds slice) {
    if (stopped()) return;
    updateRtcInterrupt();
    if (serial) serial->poll();
    if (agentSocket) {
        agentSocket->pump(*serial);
        slice = std::min(slice, std::chrono::milliseconds(10));
    }
    checkDeviceErrors();
    hyperv.service();
    if (auto delay = hyperv.serviceDelay100ns()) {
        auto milliseconds = *delay / 10000 + (*delay % 10000 != 0);
        slice = std::min(slice, std::chrono::milliseconds(std::max(uint64_t(1), milliseconds)));
    }
    slice = std::min(slice, std::chrono::milliseconds(1000));
    if (gateway) {
        gateway->pump(net->outgoing, [this](const uint8_t *data, size_t size) { net->receive(data, size); }, 0, net->resetCount);
        if (!net->error().empty()) throw std::runtime_error("Virtio network requires reset: " + net->error());
        if (auto delay = gateway->serviceDelay())
            slice = std::min(slice, std::max(std::chrono::milliseconds(1), *delay));
    }
    auto exit = vm.run(slice);
    if (exit.reason == HvfMachine::ExitReason::Canceled) return;
    if (exit.reason == HvfMachine::ExitReason::Exception && exit.exceptionClass() == 0x18) {
        unsigned reg = (exit.syndrome >> 5) & 31;
        uint64_t value = reg < 31 ? vm.reg(reg) : 0;
        if (osLocks[vm.currentCPU()].access(exit.syndrome, value)) {
            if ((exit.syndrome & 1) && reg < 31) vm.setReg(reg, value);
            vm.setPC(exit.pc + 4);
            return;
        }
    }
    if (exit.reason == HvfMachine::ExitReason::Exception && exit.exceptionClass() == 0x16) {
        ArmHyperV::Registers registers;
        for (unsigned i = 0; i < registers.size(); ++i) registers[i] = vm.reg(i);
        if (hyperv.call(unsigned(exit.syndrome & 0xffff), registers, vm.currentCPU())) {
            auto crash = hyperv.takeCrashReport();
            if (!crash.empty()) std::cerr << "Hyper-V guest crash: " << crash << '\n';
            for (unsigned i = 0; i < registers.size(); ++i) vm.setReg(i, registers[i]);
            return; // HVC exits already advance the PC, including legacy HVC #1.
        }
        auto function = uint32_t(vm.reg(0));
        uint64_t target = vm.reg(1), argument = vm.reg(2), context = vm.reg(3);
        if (!(function & 0x40000000)) { target = uint32_t(target); argument = uint32_t(argument); context = uint32_t(context); }
        ArmPsciResult result{-1};
        if ((exit.syndrome & 0xffff) == 0) {
            if (function == 0x84000003 || function == 0xc4000003) {
                result.value = target < vm.cpuCount() ? vm.startCPU(unsigned(target), argument, context) : -2;
                if (std::getenv("PCEMU_TRACE_PSCI")) std::cerr << "PSCI CPU_ON target=" << target << " entry=" << std::hex << argument << " context=" << context << std::dec << " result=" << result.value << '\n';
                if (!result.value) osLocks[unsigned(target)] = ArmOsLock{};
            } else if (function == 0x84000002) result.value = vm.stopCPU();
            else if (function == 0x84000004 || function == 0xc4000004) {
                result.value = -2;
                if (argument == 0 && target < vm.cpuCount()) result.value = vm.cpuOnline(unsigned(target)) ? 0 : 1;
                else if (argument > 0 && argument <= 3 && (target >> (8 * argument)) == 0) result.value = 0;
            } else result = armPsciCall(function, target, argument);
        }
        powerAction = result.action;
        if (!stopped()) vm.setReg(0, uint64_t(result.value));
        // Hypervisor.framework reports HVC with PC already advanced.
        return;
    }
    if (exit.reason == HvfMachine::ExitReason::Exception && exit.exceptionClass() == 0x24 &&
        (exit.syndrome & (1ull << 24)) && (exit.syndrome & (1ull << 25))) {
        unsigned reg = (exit.syndrome >> 16) & 31;
        unsigned width = 1u << ((exit.syndrome >> 22) & 3);
        bool write = exit.syndrome & (1ull << 6);
        MmioAccess access{exit.physicalAddress, write && reg < 31 ? vm.reg(reg) : 0,
                          width, write, reg == 31};
        if (mmio.access(access)) {
            if (!write && reg < 31)
                vm.setReg(reg, armMmioLoad(access.value, width, exit.syndrome & (1ull << 21),
                                          exit.syndrome & (1ull << 15)));
            vm.setPC(exit.pc + 4);
            return;
        }
    }
    std::ostringstream message;
    message << "Unhandled ARM platform exit: reason=" << unsigned(exit.reason)
            << " native_reason=" << exit.nativeReason << " cpu=" << exit.cpu << std::hex
            << " ec=" << exit.exceptionClass() << " esr=" << exit.syndrome
            << " pc=" << exit.pc << " ipa=" << exit.physicalAddress;
    if (exit.reason == HvfMachine::ExitReason::Unknown) {
        if (exit.pstateAvailable) message << " pstate=" << exit.pstate;
        else message << " pstate=unavailable";
        // All VPs are stopped. Do not replace the original failure if an
        // optional diagnostic register is unavailable on this HVF version.
        try {
            message << " context={" << cpuState()
                    << " sctlr_el1=0x" << vm.systemReg(0xc080)
                    << " ttbr0_el1=0x" << vm.systemReg(0xc100)
                    << " ttbr1_el1=0x" << vm.systemReg(0xc101)
                    << " tcr_el1=0x" << vm.systemReg(0xc102) << '}';
        } catch (const std::exception &error) { message << " context_unavailable=" << error.what(); }
    }
    throw std::runtime_error(message.str());
}
} // namespace pc
