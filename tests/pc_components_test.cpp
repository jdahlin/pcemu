#include "cmos_rtc.hpp"
#include "pc_display.hpp"
#include "machine.hpp"
#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
using namespace pc;
static unsigned checks;
static void check(bool condition) {
    ++checks;
    if (!condition) throw std::runtime_error("PC component check " + std::to_string(checks));
}
int main() {
    int64_t host = 1700000000;
    CmosRtc rtc([&] { return host; });
    rtc.configure({32 * 1024 * 1024, true, false});
    rtc.select(0x8b); check(rtc.state.index == 0x8b && rtc.readData() == 2);
    rtc.select(0x17); check(rtc.readData() == 0);
    rtc.select(0x18); check(rtc.readData() == 124);
    rtc.select(0x12); check(rtc.readData() == 0xf0);
    rtc.select(0x10); check(rtc.readData() == 0);
    rtc.configure({16 * 1024 * 1024, false, true});
    rtc.select(0x12); check(rtc.readData() == 0);
    rtc.select(0x10); check(rtc.readData() == 0x40);
    rtc.select(0x14); check(rtc.readData() == 0x21);
    rtc.setTime(1000); host += 7;
    check(rtc.now() == 1007);
    auto now = std::time_t(rtc.now());
    auto local = *std::localtime(&now);
    rtc.select(0); check(rtc.readData() == unsigned((local.tm_sec / 10) * 16 + local.tm_sec % 10));
    rtc.reset(); check(rtc.state.index == 0 && rtc.now() == 1007);
    rtc.select(0x7f); check(rtc.readData() == 0);

    Machine machine("", "", 2 * 1024 * 1024);
    PcDisplay display;
    std::vector<uint8_t> rom(2048);
    const uint8_t glyph[] = {0x30, 0x78, 0xcc, 0xcc, 0xfc, 0xcc, 0xcc, 0};
    std::copy(std::begin(glyph), std::end(glyph), rom.begin() + 65 * 8);
    display.loadFont(rom);
    machine.cpu.mem[0xb8000] = 'A'; machine.cpu.mem[0xb8001] = 0x1e;
    std::vector<uint32_t> pixels(640 * 400);
    display.render(machine, pixels, 640, 400, 500);
    check(pixels[0] == 0xff0000aa && pixels[2] == 0xffffff55);
    check(pixels[14 * 640] == 0xff0000aa);
    display.render(machine, pixels, 640, 400, 0);
    check(pixels[14 * 640] == 0xffaaaaaa); // GUI cursor blinks; captures omit it.
    auto path = std::filesystem::temp_directory_path() / ("pcemu-display-" + std::to_string(getpid()) + ".ppm");
    display.save(machine, path.string(), false);
    std::ifstream frame(path, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(frame)), {});
    frame.close(); std::filesystem::remove(path);
    const std::string header = "P6\n640 400\n255\n";
    check(bytes.size() == header.size() + 640 * 400 * 3);
    check(bytes.substr(0, header.size()) == header);
    check(bytes.substr(header.size(), 3) == std::string("\0\0\252", 3));
    check(bytes.substr(header.size() + 6, 3) == std::string("\377\377\125", 3));
    machine.cpu.mem[0x449] = 3;
    machine.cpu.mem[0x44a] = 40;
    machine.cpu.mem[0x484] = 24;
    machine.cpu.mem[0x450] = 4; machine.cpu.mem[0x451] = 2;
    display.refresh(machine, true);
    check(machine.columns == 40 && machine.rows == 25 && machine.cursor == 84);
    std::cout << checks << " CMOS/RTC and presentation checks passed\n";
}
