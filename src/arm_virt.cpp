#include "arm_virt.hpp"
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
namespace pc {
namespace fs = std::filesystem;
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
    : config(c), vm(ramBytes, true), nor(load(c.variableTemplate, 0xc0000)),
      fwCfg(vm.ram(), HvfMachine::ramBase) {
    fwCfg.addFile("etc/ramfb", std::vector<uint8_t>(28), true);
    fwCfg.addFile("etc/acpi/tables", load(c.acpiDirectory + "/tables", 64 * 1024));
    fwCfg.addFile("etc/acpi/rsdp", load(c.acpiDirectory + "/rsdp", 36));
    fwCfg.addFile("etc/table-loader", load(c.acpiDirectory + "/table-loader", 64 * 1024));
    const auto working = fs::absolute(c.variables).lexically_normal();
    config.variables = working.string();
    const auto original = fs::absolute(c.variableTemplate).lexically_normal();
    if (working == original || (fs::exists(working) && fs::equivalent(working, original)))
        throw std::runtime_error("UEFI variables must use a private file, not the firmware template");
    fs::create_directories(working.parent_path());
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
        if (!c.disk.empty()) {
            const auto parent = fs::canonical(c.disk).string();
            std::ifstream probe(parent, std::ios::binary);
            std::array<char, 8> signature{};
            probe.read(signature.data(), signature.size());
            bool direct = c.diskOverlay.empty() && std::string(signature.data(), signature.size()) == "vhdxfile";
            auto overlay = direct ? parent : c.diskOverlay.empty() ? config.variables + ".pcd" : fs::absolute(c.diskOverlay).string();
            fs::create_directories(fs::path(overlay).parent_path());
            if (!direct && fs::exists(overlay) && fs::equivalent(parent, overlay))
                throw std::runtime_error("Virtio disk overlay must differ from immutable parent");
            diskLock = open((overlay + ".lock").c_str(), O_CREAT | O_RDWR, 0600);
            if (diskLock < 0 || flock(diskLock, LOCK_EX | LOCK_NB) != 0)
                throw std::runtime_error("Virtio disk is already in use or cannot be locked");
            if (!direct && !fs::exists(overlay)) Disk::createOverlay(overlay, parent);
            disk = std::make_unique<Disk>(overlay);
            if (!direct && (!disk->isOverlay() || fs::canonical(disk->backingPath()).string() != parent))
                throw std::runtime_error("Virtio disk overlay belongs to a different parent");
            if (c.diskController == "nvme") {
                nvme = std::make_unique<Nvme>(*disk, vm.ram(), HvfMachine::ramBase,
                                            [this](bool level) { vm.setSPI(48, level); });
            } else {
                block = std::make_unique<VirtioBlock>(*disk, vm.ram(), HvfMachine::ramBase,
                                                     [this](bool level) { if (pci) pci->irq(level); else vm.setSPI(48, level); });
                if (c.virtioTransport == "pci")
                    pci = std::make_unique<VirtioPciBlock>(*block, [this](bool level) { vm.setSPI(48, level); });
            }
        } else if (!c.diskOverlay.empty()) throw std::runtime_error("--disk-overlay requires --disk");
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
        throw;
    }
}
ArmVirtMachine::~ArmVirtMachine() {
    if (variableLock >= 0) close(variableLock);
    if (diskLock >= 0) close(diskLock);
}
void ArmVirtMachine::saveVariables() {
    if (disk) disk->flush();
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
std::string ArmVirtMachine::framebuffer() const {
    auto bytes = fwCfg.file(0x20);
    auto field = [&](size_t offset, size_t count) {
        uint64_t value = 0;
        for (size_t i = 0; i < count; ++i) value = (value << 8) | bytes[offset + i];
        return value;
    };
    auto address = field(0, 8), format = field(8, 4), flags = field(12, 4);
    auto width = field(16, 4), height = field(20, 4), stride = field(24, 4);
    if (!address) return "inactive";
    if (format != 0x34325258 || flags || !width || !height || width > 4096 || height > 4096 ||
        stride < width * 4 || stride > 16384 || address < HvfMachine::ramBase ||
        address - HvfMachine::ramBase > ramBytes ||
        height * stride > ramBytes - (address - HvfMachine::ramBase))
        return "invalid";
    return std::to_string(width) + "x" + std::to_string(height) + "x32";
}
void ArmVirtMachine::saveFramebuffer(const std::string &path) const {
    auto mode = framebuffer();
    if (mode == "inactive" || mode == "invalid") throw std::runtime_error("No valid GOP framebuffer");
    auto bytes = fwCfg.file(0x20);
    auto field = [&](size_t offset, size_t count) {
        uint64_t value = 0;
        for (size_t i = 0; i < count; ++i) value = (value << 8) | bytes[offset + i];
        return value;
    };
    auto address = field(0, 8), width = field(16, 4), height = field(20, 4), stride = field(24, 4);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << "P6\n" << width << ' ' << height << "\n255\n";
    std::vector<uint8_t> row(width * 4), rgb(width * 3);
    for (uint64_t y = 0; y < height; ++y) {
        vm.readMemory(address + y * stride, row);
        for (uint64_t x = 0; x < width; ++x) {
            rgb[x * 3] = row[x * 4 + 2]; rgb[x * 3 + 1] = row[x * 4 + 1]; rgb[x * 3 + 2] = row[x * 4];
        }
        output.write(reinterpret_cast<const char *>(rgb.data()), rgb.size());
    }
    output.close();
    if (!output) throw std::runtime_error("Cannot save GOP framebuffer: " + path);
}
std::string ArmVirtMachine::cpuState() const {
    auto elr = vm.systemReg(0xc201);
    std::ostringstream output;
    output << std::hex << "pc=0x" << vm.pc() << " esr_el1=0x" << vm.systemReg(0xc290)
           << " elr_el1=0x" << elr << " far_el1=0x" << vm.systemReg(0xc300)
           << " spsr_el1=0x" << vm.systemReg(0xc200);
    auto dump = [&](const char *name, uint64_t address) {
        if (address < HvfMachine::ramBase + 16 || address - HvfMachine::ramBase > ramBytes - 48) return;
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
void ArmVirtMachine::run(std::chrono::milliseconds slice) {
    if (stopped()) return;
    auto exit = vm.run(slice);
    if (exit.reason == HvfMachine::ExitReason::Canceled) return;
    if (exit.reason == HvfMachine::ExitReason::Exception && exit.exceptionClass() == 0x18) {
        unsigned reg = (exit.syndrome >> 5) & 31;
        uint64_t value = reg < 31 ? vm.reg(reg) : 0;
        if (osLock.access(exit.syndrome, value)) {
            if ((exit.syndrome & 1) && reg < 31) vm.setReg(reg, value);
            vm.setPC(exit.pc + 4);
            return;
        }
    }
    if (exit.reason == HvfMachine::ExitReason::Exception && exit.exceptionClass() == 0x16) {
        auto result = (exit.syndrome & 0xffff) == 0
            ? armPsciCall(vm.reg(0), vm.reg(1), vm.reg(2)) : ArmPsciResult{-1};
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
        if (exit.physicalAddress >= 0x09020000 && exit.physicalAddress < 0x09020018) {
            unsigned offset = exit.physicalAddress - 0x09020000;
            if (write) fwCfg.write(offset, reg < 31 ? vm.reg(reg) : 0, width);
            else {
                auto value = armMmioLoad(fwCfg.read(offset, width), width,
                    exit.syndrome & (1ull << 21), exit.syndrome & (1ull << 15));
                if (reg < 31) vm.setReg(reg, value);
            }
            vm.setPC(exit.pc + 4);
            return;
        }
        if (exit.physicalAddress >= 0x04000000 && exit.physicalAddress < 0x040c0000 && write && width == 4) {
            nor.write(exit.physicalAddress - 0x04000000, reg < 31 ? uint32_t(vm.reg(reg)) : 0);
            auto [begin, end] = nor.takeDirty();
            if (end > begin) vm.writeMemory(0x04000000 + begin, nor.bytes().subspan(begin, end - begin));
            vm.setPC(exit.pc + 4);
            return;
        }
        if ((exit.physicalAddress >= 0x3f000000 && exit.physicalAddress < 0x3f100000) ||
            (pci && pci->contains(exit.physicalAddress, width)) ||
            (nvme && nvme->contains(exit.physicalAddress, width))) {
            bool ecam = exit.physicalAddress >= 0x3f000000;
            if (write) {
                if (nvme) {
                    if (ecam) nvme->configWrite(exit.physicalAddress - 0x3f000000, reg < 31 ? vm.reg(reg) : 0, width);
                    else nvme->write(exit.physicalAddress, reg < 31 ? vm.reg(reg) : 0, width);
                } else if (pci) {
                    if (ecam) pci->configWrite(exit.physicalAddress - 0x3f000000, reg < 31 ? vm.reg(reg) : 0, width);
                    else pci->write(exit.physicalAddress, reg < 31 ? vm.reg(reg) : 0, width);
                }
            } else {
                uint64_t value = nvme ? (ecam ? nvme->configRead(exit.physicalAddress - 0x3f000000, width)
                                                   : nvme->read(exit.physicalAddress, width))
                    : pci ? (ecam ? pci->configRead(exit.physicalAddress - 0x3f000000, width)
                                                  : pci->read(exit.physicalAddress, width)) : UINT64_MAX;
                value = armMmioLoad(value, width, exit.syndrome & (1ull << 21), exit.syndrome & (1ull << 15));
                if (reg < 31) vm.setReg(reg, value);
            }
            if (block && !block->error().empty()) throw std::runtime_error("Virtio device requires reset: " + block->error());
            vm.setPC(exit.pc + 4); return;
        }
        if (exit.physicalAddress >= 0x0a000000 && exit.physicalAddress < 0x0a000200) {
            unsigned offset = exit.physicalAddress - 0x0a000000;
            auto *mmio = pci ? nullptr : block.get();
            if (write) {
                if (mmio) mmio->write(offset, reg < 31 ? vm.reg(reg) : 0, width);
            } else {
                uint64_t value = mmio ? mmio->read(offset, width)
                                      : (offset == 0 ? 0x74726976u : offset == 4 ? 2u : 0u);
                value = armMmioLoad(value, width, exit.syndrome & (1ull << 21), exit.syndrome & (1ull << 15));
                if (reg < 31) vm.setReg(reg, value);
            }
            if (block && !block->error().empty())
                throw std::runtime_error("Virtio device requires reset: " + block->error());
            vm.setPC(exit.pc + 4);
            return;
        }
        if (exit.physicalAddress >= 0x09000000 && exit.physicalAddress < 0x09001000 && width <= 4) {
            unsigned offset = exit.physicalAddress - 0x09000000;
            if (offset % width) throw std::runtime_error("Unaligned PL011 access");
            if (write) uart.write(offset, reg < 31 ? uint32_t(vm.reg(reg)) : 0);
            else {
                uint64_t value = armMmioLoad(uart.read(offset), width,
                                             exit.syndrome & (1ull << 21), exit.syndrome & (1ull << 15));
                if (reg < 31) vm.setReg(reg, value);
            }
            vm.setSPI(33, uart.irq());
            vm.setPC(exit.pc + 4);
            return;
        }
    }
    std::ostringstream message;
    message << "Unhandled ARM platform exit: reason=" << unsigned(exit.reason) << std::hex
            << " ec=" << exit.exceptionClass() << " esr=" << exit.syndrome
            << " pc=" << exit.pc << " ipa=" << exit.physicalAddress;
    throw std::runtime_error(message.str());
}
} // namespace pc
