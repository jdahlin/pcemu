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
    {
        check(HostCursor::monoData(0x33, 0x55) == 0x99 && HostCursor::monoMask(0x33) == 0xcc);
        // AND/XOR: black, white, transparent and inverted pixels map to SDL.
        Machine vm("", "", 2 * 1024 * 1024);
        auto &c = vm.hostCursor;
        std::array<uint8_t, 28> shape{}; // 8x8 monochrome, hotspot 2,3.
        shape[0]=2; shape[2]=3; shape[4]=8; shape[6]=8; shape[8]=1;
        shape[10]=shape[11]=1;
        auto upload = [&](unsigned bytes) {
            vm.out(0x5659, 0xc501, 2);
            for (unsigned i=0; i<bytes; ++i) vm.out(0x565a, shape[i], 1);
            vm.out(0x5659, 0xc502, 2);
        };
        upload(27); check(!c.active); // No partial shape exposed.
        upload(28); check(c.active && c.visible && c.size==28 && c.shape[0]==2);
        HostCursor colors;
        colors.shape[4]=colors.shape[6]=colors.shape[10]=1;
        for (unsigned bpp : {8u,16u,24u,32u}) {
            colors.shape[11]=bpp;
            check(HostCursor::length(colors.shape)==13+bpp/8);
            colors.shape[13]= bpp==8 ? 7 : bpp==16 ? 0x1f : 0x56;
            colors.shape[14]= bpp==16 ? 0xf8 : 0x34;
            colors.shape[15]=0x12;
            colors.palette[7]=0x123456;
            check(colors.colorPixel(0,0)==(bpp==16 ? 0xffff00ffu : 0xff123456u));
            colors.shape[12]=0x80;
            check(!(colors.colorPixel(0,0)>>24));
            colors.shape[12]=0;
        }
        auto revision = c.revision;
        shape[4]=65; upload(28); check(c.revision==revision && c.shape[4]==8);
        shape[4]=8; shape[0]=8; upload(28); check(c.revision==revision);
        vm.out(0x5659, 0xc500, 1); check(c.visible); // Wrong width ignored.
        vm.out(0x5659, 0xc500, 2); check(c.active && !c.visible);
        vm.out(0x5659, 0xc501, 2);
        for (unsigned i=0; i<HostCursor::capacity+1; ++i) vm.out(0x565a, 0, 1);
        vm.out(0x5659, 0xc502, 2); check(!c.uploading && !c.visible);
    }

    {
        Machine vm("", "", 2 * 1024 * 1024);
        auto call = [&](unsigned command, unsigned argument) {
            vm.cpu.r[AX] = AbsoluteMouse::magic;
            vm.cpu.r[BX] = argument;
            vm.cpu.r[CX] = command;
            vm.cpu.r[DX] = AbsoluteMouse::port;
            return vm.in(AbsoluteMouse::port, 4);
        };
        check(call(10, 0) == 6 && vm.cpu.r[BX] == AbsoluteMouse::magic);
        check(!vm.absoluteMouse.active());
        call(41, 0x45414552); check(call(40, 0) == 1);
        check(call(39, 1) == 0x3442554a);
        call(41, 0x53424152); check(vm.absoluteMouse.active());
        vm.ps2.mouse().enabled = true;
        vm.absolutePointer(65535, 0, -1, 3);
        check(vm.ps2.mouse().packets.size() == 3 && vm.ps2.mouse().packets[1] == 1 &&
              (vm.ps2.mouse().packets[0] & 3) == 3);
        check(call(40, 0) == 4);
        check(call(39, 4) == 0x30 && vm.cpu.r[BX] == 65535 &&
              vm.cpu.r[CX] == 0 && vm.cpu.r[DX] == 0xffffffff);
        vm.absolutePointer(-1, 70000, 0, 4);
        check(call(39, 1) == 8 && call(39, 1) == 0 && call(39, 1) == 65535);
        check(call(39, 1) == 0);
        call(39, 4); check(!vm.absoluteMouse.active() && call(40, 0) == 0xffff0000);
        call(41, 0x45414552); call(39, 1); call(41, 0x53424152);
        for (unsigned i = 0; i < 1000; ++i) vm.absolutePointer(i, i, 0, 0);
        check(vm.absoluteMouse.active() && call(40, 0) == 4);
        check(call(39, 4) == 0 && vm.cpu.r[BX] == 999 && vm.cpu.r[CX] == 999);
        for (unsigned i = 0; i < 33; ++i) vm.absolutePointer(i, i, 0, i & 1);
        check(!vm.absoluteMouse.active() && vm.absoluteMouse.count <= 128);
        vm.absoluteMouse.reset();
        vm.cpu.segment(CS, 0); vm.cpu.ip = 0x600;
        vm.cpu.mem[0x600] = 0x66; vm.cpu.mem[0x601] = 0xed; // IN EAX,DX
        vm.cpu.r[AX] = AbsoluteMouse::magic; vm.cpu.r[BX] = ~AbsoluteMouse::magic;
        vm.cpu.r[CX] = 10; vm.cpu.r[DX] = AbsoluteMouse::port;
        vm.cpu.step();
        check(vm.cpu.r[AX] == 6 && vm.cpu.r[BX] == AbsoluteMouse::magic);
        vm.cpu.r[AX] = 0;
        check(vm.in(AbsoluteMouse::port, 4) == 0xffffffff);
        vm.cpu.r[AX] = AbsoluteMouse::magic;
        check(vm.in(AbsoluteMouse::port, 2) == 0xffffffff);
    }

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

    // A single temporary boot sector is enough to exercise firmware handoff.
    auto bootPath = std::filesystem::temp_directory_path() /
                    ("pcemu-bios-" + std::to_string(getpid()) + ".img");
    std::vector<char> sector(512, 0);
    sector[0] = char(0xf4); // HLT: the disk itself prints nothing.
    sector[510] = char(0x55); sector[511] = char(0xaa);
    { std::ofstream file(bootPath, std::ios::binary); file.write(sector.data(), sector.size()); }
    {
        Machine bootMachine(bootPath.string(), bootPath.string(), 2 * 1024 * 1024);
        for (bool floppy : {false, true}) {
            bootMachine.boot(floppy);
            auto text = bootMachine.text();
            check(text.find("PCEmu BIOS") == 0);
            check(text.find("System memory: 2048 KB") != std::string::npos);
            check(text.find("Hard disk: detected") != std::string::npos);
            check(text.find(floppy ? "Booting from floppy disk" : "Booting from hard disk") != std::string::npos);
            check(bootMachine.cpu.ip == 0x7c00);
            check(bootMachine.cpu.r[DX] == (floppy ? 0u : 0x80u));
            check(bootMachine.cpu.read(0x7c00, 1) == 0xf4);
            check(bootMachine.cpu.read(0x450, 2) == 0x0900);
        }
    }
    std::filesystem::remove(bootPath);
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
    // Both presentation entry points must preserve reference VGA pixels,
    // including odd row lengths and a non-default DAC mask.
    for (unsigned mode : {0x12u, 0x13u}) {
        machine.vga.mode(mode, true);
        machine.vga.width = 19; machine.vga.height = 9; machine.vga.dacMask = 15;
        for (unsigned p = 0; p < 4; ++p)
            for (unsigned a = 0; a < 100; ++a) machine.vga.planes[p][a] = a * 31 + p * 53;
        pixels.resize(19 * 9);
        display.render(machine, pixels, 19, 9, 0);
        display.save(machine, path.string(), false);
        std::ifstream graphicsFrame(path, std::ios::binary);
        std::string data((std::istreambuf_iterator<char>(graphicsFrame)), {});
        graphicsFrame.close(); std::filesystem::remove(path);
        const std::string graphicsHeader = "P6\n19 9\n255\n";
        check(data.size() == graphicsHeader.size() + 19 * 9 * 3);
        check(data.substr(0, graphicsHeader.size()) == graphicsHeader);
        for (unsigned y = 0; y < 9; ++y) for (unsigned x = 0; x < 19; ++x) {
            auto expected = machine.vga.pixel(x, y);
            check(pixels[y * 19 + x] == expected);
            auto at = graphicsHeader.size() + (y * 19 + x) * 3;
            check(uint8_t(data[at]) == uint8_t(expected >> 16) &&
                  uint8_t(data[at + 1]) == uint8_t(expected >> 8) &&
                  uint8_t(data[at + 2]) == uint8_t(expected));
        }
    }
    machine.cpu.mem[0x449] = 3;
    machine.cpu.mem[0x44a] = 40;
    machine.cpu.mem[0x484] = 24;
    machine.cpu.mem[0x450] = 4; machine.cpu.mem[0x451] = 2;
    display.refresh(machine, true);
    check(machine.columns == 40 && machine.rows == 25 && machine.cursor == 84);
    std::cout << checks << " CMOS/RTC and presentation checks passed\n";
}
