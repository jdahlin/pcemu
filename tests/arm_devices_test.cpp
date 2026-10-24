#include "arm_devices.hpp"
#include "arm_fw_cfg.hpp"
#include <array>
#include <iostream>
#include <stdexcept>
using namespace pc;
static unsigned checks = 0;
static void check(bool condition) {
    ++checks;
    if (!condition) throw std::runtime_error("ARM device check " + std::to_string(checks));
}
template <class F> static void rejects(F f) {
    bool rejected = false;
    try { f(); } catch (const std::runtime_error &) { rejected = true; }
    check(rejected);
}
int main() {
    check(armPsciCall(0x84000000, 0, 0).value == 2);
    check(armPsciCall(0xc4000003, 0, 0).value == -4);
    check(armPsciCall(0xc4000003, 1, 0).value == -2);
    check(armPsciCall(0x84000003, 1ull << 32, 0).value == -4);
    check(armPsciCall(0xc4000003, 1ull << 32, 0).value == -2);
    check(armPsciCall(0xc4000004, 0, 0).value == 0);
    check(armPsciCall(0xc4000004, 0, 1).value == -2);
    check(armPsciCall(0xc4000004, 1, 0).value == -2);
    check(armPsciCall(0x84000006, 0, 0).value == 2);
    check(armPsciCall(0xc4000001, 0, 0).value == 0);
    check(armPsciCall(0xc4000001, 0x10000, 0).value == -2);
    check(armPsciCall(0x84000002, 0, 0).action == ArmPowerAction::Off);
    check(armPsciCall(0x84000008, 0, 0).action == ArmPowerAction::Off);
    check(armPsciCall(0x84000009, 0, 0).action == ArmPowerAction::Reset);
    check(armPsciCall(0x8400000a, 0, 0).value == -1);
    check(armPsciCall(0x80000000, 0, 0).value == -1);
    std::vector<uint8_t> memory(4096);
    ArmFwCfg cfg(memory, 0x40000000);
    check(cfg.read(0, 4) == 0x554d4551);
    cfg.write(8, 0x100, 2);
    check(cfg.read(0, 4) == 3);
    auto key = cfg.addFile("etc/ramfb", std::vector<uint8_t>(28), true);
    check(key == 0x20);
    rejects([&] { cfg.addFile("etc/ramfb", {}); });
    cfg.write(8, 0x1900, 2);
    check(cfg.read(0, 4) == 0x01000000);
    check(cfg.read(0, 4) == 0x1c000000);
    check(cfg.read(0, 2) == 0x2000);
    auto putBe = [&](size_t offset, uint64_t value, size_t count) {
        for (size_t i = count; i; --i) { memory[offset + i - 1] = value; value >>= 8; }
    };
    auto transfer = [&](uint32_t control, uint32_t length, uint64_t target) {
        putBe(0, control, 4); putBe(4, length, 4); putBe(8, target, 8);
        cfg.write(16, 0, 4); cfg.write(20, 0x40, 4);
        return memory[3];
    };
    memory[128] = 0x42;
    check(transfer((key << 16) | 8 | 16, 1, 0x40000080) == 0);
    check(cfg.file(key)[0] == 0x42);
    check(transfer((key << 16) | 8 | 2, 32, 0x40000100) == 0);
    check(memory[256] == 0x42 && memory[284] == 0 && memory[287] == 0);
    check(transfer((key << 16) | 8 | 16, 29, 0x40000080) == 1);
    check(cfg.file(key)[0] == 0x42);
    check(transfer(8 | 16, 1, 0x40000080) == 1); // Read-only signature.
    check(transfer(8 | 2, 32, 0x40000fff) == 1);
    check(transfer((key << 16) | 8 | 4, 27, 0) == 0);
    check(transfer(16, 2, 0x40000080) == 1); // No partial write beyond item.
    cfg.write(8, 0xffff, 2); check(cfg.read(0, 8) == 0);
    check(cfg.read(16, 8) == 0x47464320554d4551ull);
    rejects([&] { cfg.write(16, 0xffffffffffffffffull, 8); }); // Out-of-RAM descriptor is a platform error.
    std::vector<uint8_t> blank(0x80000, 0xff);
    ArmNorFlash flash(blank);
    flash.write(0, 0x00900090);
    check(flash.bytes()[0] == 0x89 && flash.bytes()[4] == 0x18 && flash.bytes()[8] == 0);
    flash.write(0, 0x00ff00ff);
    check(flash.bytes()[0] == 0xff && flash.bytes()[8] == 0xff);
    flash.write(0, 0x00700070);
    check(flash.bytes()[0] == 0x80 && flash.bytes()[2] == 0x80);
    check(flash.storage()[0] == 0xff);
    flash.write(0, 0x00ff00ff);
    flash.write(0, 0x00400040);
    flash.write(0, 0x12345678);
    check(flash.storage()[0] == 0x78 && flash.storage()[3] == 0x12);
    check(flash.bytes()[0] == 0x80);
    flash.write(0, 0x00ff00ff);
    check(flash.bytes()[0] == 0x78 && flash.bytes()[3] == 0x12);
    flash.write(0, 0x00400040); flash.write(0, 0xffffffff); flash.write(0, 0x00ff00ff);
    check(flash.storage()[0] == 0x78); // Programming cannot set zero bits.
    flash.write(0x40000, 0x00e800e8);
    flash.write(0x40000, 0x00010001); // Two x32 words.
    flash.write(0x40020, 0xaabbccdd); flash.write(0x40024, 0x11223344);
    check(flash.storage()[0x40020] == 0xff); // Buffer is not committed early.
    flash.write(0x40000, 0x00d000d0); flash.write(0x40000, 0x00ff00ff);
    check(flash.storage()[0x40020] == 0xdd && flash.storage()[0x40024] == 0x44);
    flash.write(0, 0x00200020); flash.write(0, 0x00d000d0); flash.write(0, 0x00ff00ff);
    check(flash.storage()[0] == 0xff && flash.storage()[0x3ffff] == 0xff);
    check(flash.storage()[0x40020] == 0xdd); // Erase stays within its block.
    flash.write(0, 0x00600060); flash.write(0, 0x00d000d0); flash.write(0, 0x00ff00ff);
    check(flash.storage()[0x40020] == 0xdd);
    auto dirty = flash.takeDirty(); check(dirty.first == 0 && dirty.second >= 0x40028);
    check(flash.takeDirty().first == SIZE_MAX);
    rejects([&] { flash.write(1, 0xff); });
    rejects([&] { flash.write(UINT64_MAX - 3, 0xff); });
    rejects([&] { flash.write(0x80000, 0xff); });
    rejects([&] { flash.write(0, 0x1234); });
    rejects([] { std::array<uint8_t, 3> bytes{}; ArmNorFlash invalid(bytes); });

    check(armMmioLoad(0x1234, 1, false, true) == 0x34);
    check(armMmioLoad(UINT64_MAX, 2, false, false) == 0xffff);
    check(armMmioLoad(0x80, 1, true, true) == 0xffffffffffffff80ull);
    check(armMmioLoad(0x8001, 2, true, false) == 0xffff8001);
    check(armMmioLoad(0x80000000, 4, true, true) == 0xffffffff80000000ull);
    check(armMmioLoad(UINT64_MAX, 8, false, true) == UINT64_MAX);
    ArmPl011 uart;
    check(uart.read(0x18) == 0x90);
    uart.write(0, 'X'); check(uart.output().empty()); // UART disabled at reset.
    uart.write(0x30, 0x301); uart.write(0, 'A'); uart.write(0, 0x142);
    check(uart.output() == "AB");
    uart.input("xy"); check(uart.read(0x18) == 0x80);
    check(uart.read(0) == 'x' && uart.read(0) == 'y');
    check(uart.read(0x18) == 0x90 && uart.read(0) == 0);
    uart.write(0x24, 0x1234); uart.write(0x28, 0xff); uart.write(0x2c, 0x70);
    check(uart.read(0x24) == 0x1234 && uart.read(0x28) == 63 && uart.read(0x2c) == 0x70);
    check(uart.read(0xfe0) == 0x11 && uart.read(0xffc) == 0xb1);
    check(uart.read(0x40) == 0 && !uart.irq());
    uart.write(0x38, 0x50);
    uart.input("z");
    check(uart.irq() && uart.read(0x40) == 0x50);
    uart.write(0x44, 0x7ff);
    check(uart.irq()); // Clearing cannot suppress a nonempty RX level.
    check(uart.read(0) == 'z' && !uart.irq());
    uart.write(0x38, 0x20);
    check(uart.irq() && uart.read(0x40) == 0x20);
    uart.write(0x30, 0);
    check(!uart.irq());
    uart.write(0x30, 0x301);
    uart.write(0x38, 0);
    check(!uart.irq());
    uart.write(0x34, 0x12);
    check(uart.read(0x34) == 0x12);
    rejects([&] { uart.input(std::string(4097, 'x')); });
    rejects([&] { uart.read(0x123); });
    rejects([&] { uart.write(0x123, 0); });
    std::cout << checks << " ARM device checks passed\n";
}
