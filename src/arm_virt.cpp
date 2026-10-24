#include "arm_virt.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
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
    : config(c), vm(256 * 1024 * 1024, true), nor(load(c.variableTemplate, 0xc0000)) {
    const auto working = fs::absolute(c.variables).lexically_normal();
    config.variables = working.string();
    const auto original = fs::absolute(c.variableTemplate).lexically_normal();
    if (working == original || (fs::exists(working) && fs::equivalent(working, original)))
        throw std::runtime_error("UEFI variables must use a private file, not the firmware template");
    fs::create_directories(working.parent_path());
    variableLock = open((working.string() + ".lock").c_str(), O_CREAT | O_RDWR, 0600);
    if (variableLock < 0 || flock(variableLock, LOCK_EX | LOCK_NB) != 0) {
        if (variableLock >= 0) close(variableLock);
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
        close(variableLock); variableLock = -1; throw;
    }
}
ArmVirtMachine::~ArmVirtMachine() {
    if (variableLock >= 0) close(variableLock);
}
void ArmVirtMachine::saveVariables() {
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
void ArmVirtMachine::run(std::chrono::milliseconds slice) {
    auto exit = vm.run(slice);
    if (exit.reason == HvfMachine::ExitReason::Canceled) return;
    if (exit.reason == HvfMachine::ExitReason::Exception && exit.exceptionClass() == 0x24 &&
        (exit.syndrome & (1ull << 24)) && (exit.syndrome & (1ull << 25))) {
        unsigned reg = (exit.syndrome >> 16) & 31;
        unsigned width = 1u << ((exit.syndrome >> 22) & 3);
        bool write = exit.syndrome & (1ull << 6);
        if (exit.physicalAddress >= 0x04000000 && exit.physicalAddress < 0x040c0000 && write && width == 4) {
            nor.write(exit.physicalAddress - 0x04000000, reg < 31 ? uint32_t(vm.reg(reg)) : 0);
            auto [begin, end] = nor.takeDirty();
            if (end > begin) vm.writeMemory(0x04000000 + begin, nor.bytes().subspan(begin, end - begin));
            vm.setPC(exit.pc + 4);
            return;
        }
        if (exit.physicalAddress >= 0x09000000 && exit.physicalAddress < 0x09001000 && width <= 4) {
            unsigned offset = exit.physicalAddress - 0x09000000;
            if (offset % width) throw std::runtime_error("Unaligned PL011 access");
            if (write) uart.write(offset, reg < 31 ? uint32_t(vm.reg(reg)) : 0);
            else {
                uint64_t value = uart.read(offset) & (UINT32_MAX >> (32 - width * 8));
                if (exit.syndrome & (1ull << 21)) {
                    auto sign = 1ull << (width * 8 - 1);
                    value = (value ^ sign) - sign;
                }
                if (!(exit.syndrome & (1ull << 15))) value = uint32_t(value);
                if (reg < 31) vm.setReg(reg, value);
            }
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
