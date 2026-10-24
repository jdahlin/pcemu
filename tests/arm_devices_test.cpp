#include "arm_devices.hpp"
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
    check(uart.read(0x40) == 0);
    rejects([&] { uart.input(std::string(4097, 'x')); });
    rejects([&] { uart.read(0x123); });
    rejects([&] { uart.write(0x123, 0); });
    std::cout << checks << " ARM device checks passed\n";
}
