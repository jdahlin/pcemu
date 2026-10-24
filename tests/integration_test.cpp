#include "host_key.hpp"
#include "host_keyboard.hpp"
#include "machine.hpp"
#include "ps2_test_support.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
using namespace pc;
static unsigned checks;
static void check(bool value, const char *message) {
    ++checks;
    if (!value)
        throw std::runtime_error(message);
}
int main() {
    auto point =
        std::filesystem::temp_directory_path() / ("pcemu-input-video-" + std::to_string(getpid()));
    try {
        {
            std::filesystem::create_directories(point);
            auto diskPath = point / "ports.raw";
            {
                std::ofstream disk(diskPath, std::ios::binary);
                std::array<char, 512> sector{};
                sector[510] = 0x55;
                sector[511] = char(0xaa);
                disk.write(sector.data(), sector.size());
            }
            Machine machine(diskPath.string(), "", 2 * 1024 * 1024);
            machine.ata.irq = true;
            machine.ataLine = true;
            machine.cpu.input(0x3f6, 1);
            check(machine.ata.irq && machine.ataLine,
                  "routed ATA alternate status preserves IRQ state");
            machine.cpu.input(0x1f7, 1);
            check(!machine.ata.irq && !machine.ataLine,
                  "routed ATA status acknowledges device IRQ and machine latch");
            machine.cpu.output(0x1f2, 0x23, 2);
            check(machine.cpu.input(0x1f2, 1) == 0x23 && machine.cpu.input(0x1f3, 1) == 1,
                  "wide ATA register output remains one device transaction");
            machine.nic.remote = 0x4000;
            machine.nic.remoteCount = 4;
            machine.nic.command = 0x12;
            machine.cpu.output(0x310, 0x12345678, 4);
            check(machine.nic.memory[0x4000] == 0x78 && machine.nic.memory[0x4003] == 0x12 &&
                      machine.nic.remoteCount == 0,
                  "routed NE2000 dword output preserves remote DMA width");
            machine.nicLine = true;
            machine.nic.imr = 0;
            machine.cpu.output(0x307, 0xff, 1);
            check(!machine.nicLine, "routed NE2000 writes lower the machine IRQ latch");
            machine.out(0x1234, 0x12345678, 4);
            check(machine.in(0x1234, 1) == 0x78 && machine.in(0x1237, 1) == 0x12,
                  "unmapped wide output retains consecutive byte fallback");
            machine.bindDevices();
            machine.bindDevices();
            machine.cpu.output(0x1f2, 0x45, 1);
            check(machine.cpu.input(0x1f2, 1) == 0x45,
                  "repeated binding replaces port regions without overlaps");
            machine.restart();
            machine.cpu.output(0x1f2, 0x67, 1);
            check(machine.cpu.input(0x1f2, 1) == 0x67,
                  "port callbacks retain the current ATA after reset");
        }
        std::filesystem::remove_all(point);
        {
            Machine machine("", "", 2 * 1024 * 1024);
            machine.in(0x20, 1);
            machine.in(0x40, 1);
            check(machine.ioReads == 0, "PIC and PIT data reads do not advance retrace counter");
            machine.ports[0x43] = 0x57;
            check(machine.in(0x43, 4) == 0x57 && machine.ioReads == 1,
                  "PIT control reads retain saved-port fallback and retrace counting");
            machine.ioReads = 0;
            check(machine.in(0x3da, 1) == 9, "first counted VGA status read asserts retrace");
            machine.in(0x1234, 1);
            check(machine.in(0x3da, 1) == 9 && machine.ioReads == 3,
                  "unmapped reads still influence VGA retrace parity");
            machine.out(0x20, 0xff0a, 2);
            check(machine.pic[0].mask == 0xff && machine.ports[0x21] == 0xff,
                  "wide PIC writes split across command and mask registers");
            machine.ports[0x44] = 0x42;
            machine.out(0x43, 0xaa36, 2);
            check(machine.pit.counters[0].rw == 3 && machine.pit.counters[0].mode == 3 &&
                      machine.ports[0x44] == 0x42,
                  "wide PIT control writes remain a single truncated transaction");
            machine.out(0xcf8, 0x80001000, 4);
            check(machine.in(0xcfc, 4) == 0x11111234 && machine.pci.address == 0x80001000,
                  "PCI config address and data preserve dword accesses");
            machine.out(0x3c4, 0x0f02, 2);
            check(machine.vga.seqIndex == 2 && machine.vga.seq[2] == 0x0f &&
                      machine.ports[0x3c4] == 2 && machine.ports[0x3c5] == 0x0f,
                  "wide VGA writes split index/data and retain port shadows");
            machine.out(0x1ce, 1, 2);
            machine.out(0x1cf, 640, 2);
            check(machine.in(0x1ce, 2) == 1 && machine.in(0x1cf, 2) == 640,
                  "DISPI index/data preserve whole word transactions");
            machine.out(0x64, 0x5a60, 2);
            check(ps2State(machine).keyboardControllerPending == 0x60 && machine.ports[0x65] == 0x5a,
                  "wide PS2 writes cross into the adjacent unmapped port");
            machine.out(0x70, 0x558b, 2);
            check(machine.rtc.state.index == 0x8b && machine.in(0x70, 2) == 0x8b &&
                      machine.in(0x71, 2) == 2 && machine.ports[0x71] == 0x55,
                  "CMOS index masks NMI bit for reads and splits wide writes");
            machine.out(0x61, 0x5a01, 2);
            check(machine.pit.counters[2].gate && machine.ports[0x61] == 1 &&
                      machine.ports[0x62] == 0x5a,
                  "speaker gate and adjacent byte survive wide output");
            machine.out(0x92, 0x2200, 2);
            check(!machine.cpu.a20 && machine.ports[0x93] == 0x22 && !machine.resetRequested,
                  "system control wide output splits and updates A20");
            machine.out(0x92, 3, 1);
            check(machine.cpu.a20 && machine.resetRequested,
                  "system control still requests reset through bit zero");
        }
        {
            std::deque<uint8_t> stream;
            HostKeyboard keyboard(stream);
            auto tap = [&](uint16_t key) {
                keyboard.event(key, false);
                keyboard.event(key, true);
            };
            tap(hid::KEY_F12);
            check(stream == std::deque<uint8_t>({0x58, 0xd8}), "host F12 reaches the guest");
            stream.clear();
            tap(hid::KEY_RCTRL);
            tap(hid::KEY_HOME);
            tap(hid::KEY_KP_ENTER);
            check(stream == std::deque<uint8_t>({0xe0, 0x1d, 0xe0, 0x9d, 0xe0, 0x47, 0xe0, 0xc7,
                                                 0xe0, 0x1c, 0xe0, 0x9c}),
                  "host extended keys retain prefixes and distinct keypad Enter");
            stream.clear();
            tap(hid::KEY_PRINTSCREEN);
            tap(hid::KEY_PAUSE);
            check(stream == std::deque<uint8_t>({0xe0, 0x2a, 0xe0, 0x37, 0xe0, 0xb7, 0xe0, 0xaa,
                                                 0xe1, 0x1d, 0x45, 0xe1, 0x9d, 0xc5}),
                  "Print Screen and Pause wire sequences");
            stream.clear();
            keyboard.event(hid::KEY_LCTRL, false);
            keyboard.event(hid::KEY_RALT, false);
            keyboard.release();
            check(stream == std::deque<uint8_t>({0x1d, 0xe0, 0x38, 0x9d, 0xe0, 0xb8}),
                  "focus loss releases held modifiers");
            stream.clear();
            keyboard.event(hid::KEY_LGUI, false);
            check(stream.empty(), "Command make waits for a guest chord or bare tap");
            keyboard.consumeCommand();
            keyboard.event(hid::KEY_LGUI, true);
            check(stream.empty(), "native Command shortcut does not open guest Start menu");
            tap(hid::KEY_LGUI);
            check(stream == std::deque<uint8_t>({0xe0, 0x5b, 0xe0, 0xdb}),
                  "bare Command tap is a guest Windows key");
            stream.clear();
            keyboard.event(hid::KEY_LGUI, false);
            tap(hid::KEY_R);
            keyboard.event(hid::KEY_LGUI, true);
            check(stream == std::deque<uint8_t>({0xe0, 0x5b, 0x13, 0x93, 0xe0, 0xdb}),
                  "guest Windows+R chord is preserved");
            stream.clear();
            keyboard.event(hid::KEY_LGUI, false);
            keyboard.release();
            keyboard.event(hid::KEY_LGUI, true);
            keyboard.event(hid::KEY_A, true);
            check(stream.empty(), "focus loss drops pending native Command and unmatched releases");
        }
        Machine m("", "", 2 * 1024 * 1024);
        auto &c = m.cpu;
        auto readByte = [&] {
            c.instructions += 4000;
            return m.in(0x60, 1);
        };
        auto mouseCommand = [&](uint8_t value) {
            m.out(0x64, 0xd4, 1);
            m.out(0x60, value, 1);
        };
        m.out(0x64, 0xa9, 1);
        check(readByte() == 0, "AUX interface self-test succeeds");
        mouseCommand(0xff);
        c.instructions += 4000;
        check((m.in(0x64, 1) & 0x21) == 0x21, "controller distinguishes AUX output");
        check(readByte() == 0xfa && readByte() == 0xaa && readByte() == 0,
              "mouse reset ACK/BAT/ID");
        m.pic[0].irr = 0;
        m.out(0x64, 0x60, 1);
        m.out(0x60, 0x47, 1);
        mouseCommand(0xf4);
        c.instructions += 4000;
        m.in(0x64, 1);
        check(m.pic[1].irr & 0x10, "AUX data raises IRQ12");
        check(!(m.pic[0].irr & 2), "AUX data does not raise IRQ1");
        check(readByte() == 0xfa, "enable reporting ACK");
        ps2State(m).mouse.motion(-255, 255, 1);
        c.instructions += 4000;
        check(!(m.in(0x64, 1) & 1),
              "masked host mouse reporting cannot block shared keyboard buffer");
        m.out(0x60, 0xee, 1);
        check(readByte() == 0xee, "keyboard command reply passes queued masked mouse events");
        m.pic[1].mask &= ~0x10;
        check(readByte() == 0x19 && readByte() == 1 && readByte() == 255,
              "signed 9-bit movement and left button");
        ps2State(m).mouse.motion(0, 0, 0);
        check(readByte() == 8 && readByte() == 0 && readByte() == 0,
              "button release retained without motion");
        for (unsigned rate : {200, 100, 80}) {
            mouseCommand(0xf3);
            check(readByte() == 0xfa, "sample-rate command ACK");
            mouseCommand(rate);
            check(readByte() == 0xfa, "sample-rate parameter ACK");
        }
        mouseCommand(0xf2);
        check(readByte() == 0xfa && readByte() == 3, "wheel mouse negotiation");
        ps2State(m).mouse.motion(10, -3, 2, -1);
        check(readByte() == 0x2a && readByte() == 10 && readByte() == 253 && readByte() == 255,
              "four-byte wheel packet");
        mouseCommand(0xf0);
        check(readByte() == 0xfa, "remote mode ACK");
        ps2State(m).mouse.motion(4, 5, 0);
        ps2State(m).mouse.poll(c.instructions + 100000);
        check(ps2State(m).mouse.packets.empty(), "remote mode waits for request");
        mouseCommand(0xeb);
        check(readByte() == 0xfa && readByte() == 8 && readByte() == 4 && readByte() == 5 &&
                  readByte() == 0,
              "remote read packet");
        // Exercise actual ROM code and its far callback stack, rather than only
        // testing host-side BIOS service helpers.
        ps2State(m).mouse.defaults();
        c.segment(CS, 0);
        c.ip = 0x500;
        c.segment(SS, 0);
        c.r[SP] = 0x9000;
        c.segment(ES, 0);
        c.r[BX] = 0x600;
        c.r[AX] = 0xc207;
        m.service(0x15);
        check(!(c.flags & CF), "BIOS accepts far mouse handler");
        c.r[BX] = 0x100;
        c.r[AX] = 0xc200;
        m.service(0x15);
        check(!(c.flags & CF) && ps2State(m).mouse.enabled, "BIOS enables PS/2 reporting");
        // Callback copies the four stack words to 0700, preserving BP/AX.
        const uint8_t handler[] = {0x55, 0x89, 0xe5, 0x50, 0x8b, 0x46, 0x0c, 0xa3, 0x00, 0x07, 0x8b,
                                   0x46, 0x0a, 0xa3, 0x02, 0x07, 0x8b, 0x46, 0x08, 0xa3, 0x04, 0x07,
                                   0x8b, 0x46, 0x06, 0xa3, 0x06, 0x07, 0x58, 0x5d, 0xcb};
        std::copy(std::begin(handler), std::end(handler), c.mem.begin() + 0x600);
        c.segment(DS, 0);
        c.r[AX] = 0xabcd;
        c.r[BX] = 0x1234;
        ps2State(m).mouse.motion(7, -4, 1);
        for (unsigned byte = 0; byte < 3; ++byte) {
            c.instructions += 4000;
            m.in(0x64, 1);
            c.interrupt(0x74);
            for (unsigned steps = 0; !(c.s[CS].selector == 0 && c.ip == 0x500) && steps < 200;
                 ++steps)
                c.step();
            check(c.s[CS].selector == 0 && c.ip == 0x500,
                  "IRQ12 ROM returns to interrupted instruction");
        }
        check(c.read(0x700, 2) == 0x29 && c.read(0x702, 2) == 7 && c.read(0x704, 2) == 252 &&
                  c.read(0x706, 2) == 0,
              "BIOS callback receives unsigned bytes/status/zero Z in specified order");
        check(c.r[AX] == 0xabcd && c.r[BX] == 0x1234 && c.r[SP] == 0x9000,
              "IRQ12 preserves guest registers and stack");
        m.out(0xcf8, 0x80001000, 4);
        check(m.in(0xcfc, 4) == 0x11111234,
              "PCI configuration mechanism 1 identifies display function");
        check(m.in(0xcfc, 2) == 0x1234 && m.in(0xcfe, 2) == 0x1111, "PCI byte-lane access");
        m.out(0xcf8, 0x80001010, 4);
        m.out(0xcfc, 0xffffffff, 4);
        check(m.in(0xcfc, 4) == 0xff000000, "PCI BAR sizing reports 16 MiB");
        m.out(0xcfc, VGA::lfbBase, 4);
        check(m.in(0xcfc, 4) == VGA::lfbBase, "PCI framebuffer assignment");
        m.out(0xcf8, 0x80000000, 4);
        check(m.in(0xcfc, 4) == 0xffffffff, "absent PCI function returns all ones");
        c.r[AX] = 0xb101;
        m.service(0x1a);
        check(!(c.flags & CF) && c.r[DX] == 0x20494350 && c.get(BX, 2) == 0x210,
              "PCI BIOS installation signature");
        c.r[AX] = 0xb102;
        c.r[DX] = 0x1234;
        c.r[CX] = 0x1111;
        c.r[SI] = 0;
        m.service(0x1a);
        check(!(c.flags & CF) && c.get(BX, 2) == 0x10, "PCI BIOS find video device");
        c.r[AX] = 0xb10a;
        c.r[DI] = 0x10;
        m.service(0x1a);
        check(!(c.flags & CF) && c.r[CX] == VGA::lfbBase, "PCI BIOS reads framebuffer resource");
        {
            Machine fw("", "", 2 * 1024 * 1024);
            auto &p = fw.cpu;
            unsigned checksum = 0;
            for (unsigned i = 0; i < 16; ++i)
                checksum += p.read(0xf0400 + i, 1);
            check((checksum & 255) == 0 && p.read(0xf0400, 4) == 0x5f32335f,
                  "BIOS32 directory signature and checksum");
            p.gdtBase = 0x800;
            p.gdtLimit = 0x17;
            p.write(0x808, 0x0000ffff, 4);
            p.write(0x80c, 0x00cf9a00, 4);
            p.write(0x810, 0x0000ffff, 4);
            p.write(0x814, 0x00cf9200, 4);
            p.cr[0] = 1;
            p.segment(CS, 8);
            p.segment(SS, 16);
            p.r[SP] = 0x9000;
            p.flags = 2;
            auto call = [&](uint32_t at) {
                p.push(8, 4);
                p.push(0x500, 4);
                p.ip = at;
                for (unsigned steps = 0; p.ip != 0x500 && steps < 10000; ++steps)
                    p.step();
                check(p.ip == 0x500 && p.r[SP] == 0x9000, "protected-mode PCI firmware far return");
            };
            p.r[AX] = 0x49435024;
            call(0xf0410);
            check(p.get(AX, 1) == 0 && p.r[BX] == 0xf0000 && p.r[DX] == 0x440,
                  "BIOS32 discovers protected PCI entry");
            p.r[AX] = 0xb101;
            call(0xf0440);
            check(!(p.flags & CF) && p.r[DX] == 0x20494350, "protected PCI installation signature");
            p.r[AX] = 0xb10a;
            p.r[BX] = 0x10;
            p.r[DI] = 0x10;
            call(0xf0440);
            check(!(p.flags & CF) && p.r[CX] == VGA::lfbBase,
                  "protected PCI firmware reads resource through real IN/OUT");
        }
        {
            Machine fw("", "", 2 * 1024 * 1024);
            auto &p = fw.cpu;
            p.cr[0] = 1;
            p.flags = 0x23002;
            p.tr32 = true;
            p.trBase = 0x1000;
            p.trLimit = 0x100;
            p.write(0x1066, 0x68, 2); // permit PIC ports in the V86 I/O bitmap
            p.segment(CS, 0xf000);
            p.segment(SS, 0);
            p.r[SP] = 0x9000;
            p.write(0xf0320, 0x600, 4);
            unsigned picWrites = 0;
            p.output = [&](uint16_t port, uint32_t value, int width) {
                if (port == 0x21 || port == 0xa1)
                    ++picWrites;
                fw.out(port, value, width);
            };
            auto invokeMouseBIOS = [&](unsigned enabled) {
                p.segment(CS, 0xf000);
                p.ip = 0x20a0;
                p.r[AX] = 0xc200;
                p.r[BX] = enabled << 8;
                p.push(p.flags, 2);
                p.push(0, 2);
                p.push(0x500, 2);
                for (unsigned steps = 0; !(p.s[CS].selector == 0 && p.ip == 0x500) && steps < 1000;
                     ++steps)
                    p.step();
                check(p.ip == 0x500 && p.r[SP] == 0x9000 && !(p.flags & CF),
                      "V86 mouse BIOS wrapper preserves return status and stack");
            };
            invokeMouseBIOS(1);
            check(picWrites == 2 && !(fw.pic[1].mask & 0x10),
                  "V86 mouse enable executes guest-visible PIC unmask writes");
            invokeMouseBIOS(0);
            check(picWrites == 3 && (fw.pic[1].mask & 0x10),
                  "V86 mouse disable executes guest-visible PIC mask write");
        }
        auto reg = [&](unsigned index, unsigned value) {
            m.out(0x1ce, index, 2);
            m.out(0x1cf, value, 2);
        };
        reg(0, 0xb0c0);
        check(m.in(0x1cf, 2) == 0xb0c0, "DISPI ID handshake");
        reg(4, 2);
        reg(1, 800);
        m.out(0x1ce, 1, 2);
        check(m.in(0x1cf, 2) == VGA::maxWidth, "DISPI capability query");
        reg(4, 0);
        reg(1, 800);
        reg(2, 600);
        reg(3, 32);
        reg(4, 0x41);
        check(m.vga.graphics && m.vga.width == 800 && m.vga.height == 600,
              "DISPI enables true-colour geometry");
        c.write(VGA::lfbBase, 0x123456, 4);
        check(c.read(VGA::lfbBase, 4) == 0x123456 && m.vga.pixel(0, 0) == 0xff123456,
              "CPU linear framebuffer read/write and BGR rendering");
        reg(5, 1);
        c.write(0xa0000, 0xabcdef, 4);
        check(c.read(VGA::lfbBase + 65536, 4) == 0xabcdef, "banked aperture aliases linear VRAM");
        reg(6, 1024);
        reg(8, 1);
        reg(9, 1);
        c.write(VGA::lfbBase + (1024 + 1) * 4, 0x654321, 4);
        check(m.vga.pixel(0, 0) == 0xff654321, "virtual pitch and panning");
        reg(4, 0);
        reg(3, 16);
        reg(4, 0x41);
        c.write(VGA::lfbBase, 0xf800, 2);
        check(m.vga.pixel(0, 0) == 0xffff0000, "RGB565 rendering");
        reg(4, 0);
        reg(3, 15);
        reg(4, 0x41);
        c.write(VGA::lfbBase, 0x03e0, 2);
        check(m.vga.pixel(0, 0) == 0xff00ff00, "RGB555 rendering");
        reg(4, 0);
        reg(3, 24);
        reg(4, 0x41);
        c.write(VGA::lfbBase, 0x34, 1);
        c.write(VGA::lfbBase + 1, 0x12, 1);
        check(m.vga.pixel(0, 0) == 0xff001234, "packed 24-bit rendering");
        ps2State(m).mouse.motion(13, -9, 2);
        m.pollPs2();
        m.pci.address = 0x80001010;
        m.pci.display().barProbe = true;
        m.saveSnapshot(point.string());
        Machine restored("", "", 2 * 1024 * 1024);
        restored.loadSnapshot(point.string());
        check(restored.pci.address == m.pci.address && restored.pci.display().barProbe,
              "snapshot retains PCI configuration probe state");
        check(restored.vga.vram == m.vga.vram && restored.vga.dispi == m.vga.dispi,
              "snapshot retains true-colour VRAM and mode");
        check(ps2State(restored).mouse.packets == ps2State(m).mouse.packets && ps2State(restored).mouse.dx == ps2State(m).mouse.dx &&
                  ps2State(restored).keyboardAux == ps2State(m).keyboardAux &&
                  restored.biosMouseCount == m.biosMouseCount,
              "snapshot retains pending mouse/controller packet state");
        check(std::filesystem::file_size(point / "state.bin") < 100000,
              "mostly empty VRAM compresses efficiently");
        Machine firmware("", "", 2 * 1024 * 1024);
        std::ifstream rom("roms/vgabios.bin", std::ios::binary);
        firmware.vgaRom.assign(std::istreambuf_iterator<char>(rom), {});
        firmware.initializeVga();
        auto &biosCPU = firmware.cpu;
        biosCPU.segment(CS, 0);
        biosCPU.ip = 0x500;
        biosCPU.halted = false;
        biosCPU.segment(ES, 0);
        biosCPU.r[DI] = 0x1000;
        biosCPU.write(0x1000, 0x32454256, 4);
        auto invokeVideo = [&] {
            biosCPU.interrupt(0x10);
            for (unsigned steps = 0;
                 !(biosCPU.s[CS].selector == 0 && biosCPU.ip == 0x500) && steps < 1000000; ++steps)
                biosCPU.step();
            check(biosCPU.s[CS].selector == 0 && biosCPU.ip == 0x500, "VGA ROM service returns");
        };
        biosCPU.r[AX] = 0x4f00;
        invokeVideo();
        check(biosCPU.get(AX, 2) == 0x4f && biosCPU.read(0x1000, 4) == 0x41534556 &&
                  biosCPU.read(0x1004, 2) >= 0x200,
              "real VGA ROM reports VBE 2.0+ controller");
        biosCPU.r[AX] = 0x4f01;
        biosCPU.r[CX] = 0x118;
        biosCPU.r[DI] = 0x2000;
        invokeVideo();
        check(biosCPU.get(AX, 2) == 0x4f && (biosCPU.read(0x2000, 2) & 0x80) &&
                  biosCPU.read(0x2028, 4) == VGA::lfbBase,
              "real VGA ROM mode info advertises linear framebuffer address");
        biosCPU.r[AX] = 0x4f02;
        biosCPU.r[BX] = 0x4118;
        invokeVideo();
        check(biosCPU.get(AX, 2) == 0x4f && firmware.vga.width == 1024 &&
                  firmware.vga.height == 768 && (firmware.vga.dispi[4] & 1),
              "real VGA ROM enables 1024x768 true-colour mode");
        {
            Machine scheduled("", "", 4 * 1024 * 1024);
            ps2State(scheduled).keyboardCommandByte = 3;
            scheduled.pic[0].mask = scheduled.pic[1].mask = 0;
            ps2State(scheduled).mouse.enabled = true;
            ps2State(scheduled).mouse.nextAt = 1000;
            ps2State(scheduled).keyboardNextAt = 5000;
            ps2State(scheduled).mouse.motion(5, -3, 0);
            scheduled.cpu.instructions = 999;
            scheduled.pollPs2();
            check(!ps2State(scheduled).keyboardFull && ps2State(scheduled).mouse.packets.empty() && ps2State(scheduled).mouse.dx == 5,
                  "idle input guard waits until the mouse sampling deadline");
            scheduled.cpu.instructions = 1000;
            scheduled.pollPs2();
            check(!ps2State(scheduled).keyboardFull && ps2State(scheduled).mouse.packets.size() == 3 && ps2State(scheduled).mouse.dx == 0,
                  "due mouse packet forms even while controller output is paced");
            scheduled.cpu.instructions = 4999;
            scheduled.pollPs2();
            check(!ps2State(scheduled).keyboardFull && ps2State(scheduled).mouse.packets.size() == 3,
                  "queued mouse bytes wait until the exact controller deadline");
            scheduled.cpu.instructions = 5000;
            scheduled.pollPs2();
            check(ps2State(scheduled).keyboardFull && ps2State(scheduled).keyboardAux && ps2State(scheduled).mouse.packets.size() == 2 &&
                      (scheduled.pic[1].irr & 0x10),
                  "controller deadline delivers the mouse byte and raises IRQ12 immediately");
            scheduled.in(0x60, 1);
            ps2State(scheduled).keyboardReplies.push_back(0xfa);
            scheduled.cpu.instructions = ps2State(scheduled).keyboardNextAt;
            scheduled.pollPs2();
            check(ps2State(scheduled).keyboardFull && !ps2State(scheduled).keyboardAux && ps2State(scheduled).keyboardData == 0xfa,
                  "keyboard command replies retain priority over queued mouse bytes");
            ps2State(scheduled).keyLine = false;
            scheduled.pic[0].irr = 0;
            ps2State(scheduled).keyboardCommandByte &= ~1;
            scheduled.pollPs2();
            check(!(scheduled.pic[0].irr & 2), "full output buffer honors disabled keyboard IRQ");
            ps2State(scheduled).keyboardCommandByte |= 1;
            scheduled.pollPs2();
            check(scheduled.pic[0].irr & 2, "enabling IRQ1 with full output takes effect immediately");
        }
        {
            Machine idle("", "", 4 * 1024 * 1024);
            ps2State(idle).keyboardCommandByte = 1;
            idle.pic[0].mask = 0;
            idle.pollPs2();
            ps2State(idle).scanCodes.push_back(0x1e);
            idle.pollPs2();
            check(ps2State(idle).keyboardFull && ps2State(idle).keyboardData == 0x1e && (idle.pic[0].irr & 2),
                  "host input wakes an idle controller without a stale cached deadline");
        }
        {
            Machine buttons("", "", 4 * 1024 * 1024);
            ps2State(buttons).keyboardCommandByte = 2;
            buttons.pic[1].mask = 0;
            ps2State(buttons).mouse.enabled = true;
            ps2State(buttons).mouse.nextAt = 1000000;
            ps2State(buttons).mouse.motion(0, 0, 1);
            buttons.pollPs2();
            check(ps2State(buttons).keyboardFull && ps2State(buttons).keyboardAux && (buttons.pic[1].irr & 0x10),
                  "queued mouse button transitions bypass the future movement sample deadline");
        }
        std::filesystem::remove_all(point);
        std::cout << "Passed " << checks << " input/video integration checks\n";
    } catch (const std::exception &e) {
        std::filesystem::remove_all(point);
        std::cerr << "FAIL: " << e.what() << " after " << checks << " checks\n";
        return 1;
    }
}
