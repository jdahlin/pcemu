#include "machine.hpp"
#include "pc_bios_disk.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <stdexcept>
#include "int15.hpp"
#include "pci32.hpp"
namespace pc {
void Machine::initializeFirmware() {
    // Both the conventional reset-vector alias and a warm far jump enter the
    // original firmware bootstrap service. No guest OS state is substituted.
    cpu.write(0xffff0, 0xea, 1);
    cpu.write(0xffff1, 0, 2);
    cpu.write(0xffff3, 0xf000, 2);
    cpu.write(0xf0000, 0xfecd, 2);
    cpu.write(0xf0002, 0xf4, 1);
    for (unsigned n = 0; n < 256; n++) {
        // Real-mode BIOS vector stubs remain callable through saved far pointers.
        unsigned off = 0x1000 + n * 4;
        cpu.write(n * 4, 0xf0000000 | off, 4);
        cpu.write(0xf0000 + off, 0xfeCD, 2);
        cpu.write(0xf0002 + off, 0xcf, 1);
    }
    for (unsigned n : {8u, 9u}) {
        // A V86 monitor must observe the guest OUT that acknowledges a virtual
        // hardware interrupt. A host-side PIC write cannot complete its state.
        unsigned original = 0x1000 + n * 4, extended = 0x2000 + (n - 8) * 0x20;
        cpu.write(0xf0000 + original, 0xe9, 1);
        cpu.write(0xf0001 + original, extended - original - 3, 2);
        const std::vector<uint8_t> body =
            n == 8 ? std::vector<uint8_t>{0xcd, 0xfe, 0x50, 0xb0, 0x20, 0xe6, 0x20, 0x58, 0xcf}
                   : std::vector<uint8_t>{0x50, 0xe4, 0x60, 0xcd, 0xfe, 0xb0,
                                          0x20, 0xe6, 0x20, 0x58, 0xcf};
        for (unsigned k = 0; k < body.size(); k++)
            cpu.write(0xf0000 + extended + k, body[k], 1);
    }
    // Original IRQ12 firmware: preserve registers, assemble a packet, invoke
    // the INT 15/C207 far callback with four WORD arguments, then guest EOIs.
    const uint8_t mouseIRQ[] = {
        0x60, 0x1e, 0x06, 0xe4, 0x60, 0xcd, 0xfe, // PUSHA, DS, ES, IN AL,60, private service
        0x3c, 0x01, 0x75, 0x19,                   // CMP AL,1 / JNE epilogue
        0x2e, 0xff, 0x36, 0x24, 0x03,             // PUSH CS:[0324] status
        0x2e, 0xff, 0x36, 0x26, 0x03,             // PUSH X
        0x2e, 0xff, 0x36, 0x28, 0x03,             // PUSH Y
        0x6a, 0x00,                               // PUSH 0 (Z)
        0x2e, 0xff, 0x1e, 0x20, 0x03,             // CALL FAR CS:[0320]
        0x83, 0xc4, 0x08,                         // ADD SP,8
        0xb0, 0x20, 0xe6, 0xa0, 0xe6, 0x20, 0x07, 0x1f, 0x61, 0xcf};
    for (unsigned k = 0; k < sizeof(mouseIRQ); ++k)
        cpu.write(0xf2040 + k, mouseIRQ[k], 1);
    cpu.write(0xf11d0, 0xe9, 1); // original vector 74 stub
    cpu.write(0xf11d1, 0x2040 - 0x11d0 - 3, 2);
    cpu.write(0xf1054, 0xe9, 1);
    cpu.write(0xf1055, 0x20a0 - 0x1054 - 3, 2);
    for (unsigned k = 0; k < sizeof(int15Firmware); ++k)
        cpu.write(0xf20a0 + k, int15Firmware[k], 1);
    cpu.write(0xf032a, 0, 1);
    cpu.write(0xf0320, 0, 4);
    biosMouseCount = 0;
    const uint8_t directory[16] = {'_', '3', '2', '_', 0x10, 0x04, 0x0f, 0, 0, 1, 0, 0, 0, 0, 0, 0};
    uint8_t sum = 0;
    for (unsigned k = 0; k < sizeof(directory); ++k) {
        cpu.write(0xf0400 + k, directory[k], 1);
        sum += directory[k];
    }
    cpu.write(0xf040a, uint8_t(-sum), 1);
    for (unsigned k = 0; k < sizeof(pci32Firmware); ++k)
        cpu.write(0xf0410 + k, pci32Firmware[k], 1);
    cpu.write(0x410, 0x0024 | (floppy ? 1 : 0), 2);
    cpu.write(0x413, 640, 2);
    cpu.write(0x417, 0, 2);
    cpu.write(0x41a, 0x1e, 2);
    cpu.write(0x41c, 0x1e, 2);
    cpu.write(0x480, 0x1e, 2);
    cpu.write(0x482, 0x3e, 2);
    cpu.write(0x449, 3, 1);
    cpu.write(0x44a, 80, 2);
    cpu.write(0x44c, 4096, 2);
    cpu.write(0x463, 0x3d4, 2);
    cpu.write(0x484, 24, 1);
    cpu.write(0x485, 16, 2);
    cpu.write(0x475, disk ? 1 : 0, 1);
    // Configuration table returned by INT 15h/C0h: AT, no coprocessor.
    cpu.write(0xf0300, 8, 2);
    cpu.write(0xf0302, 0xfc, 1);
    cpu.write(0xf0303, 1, 1);
    cpu.write(0xf0304, 0, 1);
    cpu.write(0xf0305, 0x24, 1);
    for (unsigned i = 0; i < 80 * 25; i++)
        cpu.write(0xb8000 + i * 2, 0x0720, 2);
    // No 80387 is advertised in the equipment word. Leave EM clear as on a
    // real 386 reset: DOS presence probes must observe unanswered ESC cycles,
    // rather than #NM without an installed software-emulation handler.
    cpu.cr[0] = 0x10;
    pic[0].mask = 0xfc;
    pic[1].base = 0x70;
    ports[0x21] = 0xfc;
    ports[0xa1] = 0xff;
}
void Machine::boot(bool fd) {
    bootFromFloppy = fd;
    auto *d = fd ? floppy.get() : disk.get();
    if (!d)
        throw std::runtime_error("No boot disk");
    if (!biosDiskTransfer(*d, cpu, false, 0, 1, 0x7c00))
        throw std::runtime_error("Cannot read boot sector");
    if (cpu.read(0x7dfe, 2) != 0xaa55)
        throw std::runtime_error("Invalid boot signature");
    for (unsigned seg = 0; seg < 6; seg++)
        cpu.segment(seg, 0);
    cpu.ip = 0x7c00;
    cpu.r[DX] = fd ? 0 : 0x80;
    cpu.r[SP] = 0x7c00;
    cpu.flags = 2;
}
void Machine::initializeVga() {
    vgaOnReset = true;
    // The option-ROM shadow contains mutable runtime state. A firmware restart
    // repopulates it from the immutable asset before reinitializing the device.
    for (size_t i = 0; i < vgaRom.size() && 0xc0000 + i < cpu.mem.size(); i++)
        cpu.write(0xc0000 + i, vgaRom[i], 1);
    cpu.segment(CS, 0);
    cpu.ip = 0x600;
    cpu.r[SP] = 0x7c00;
    cpu.r[AX] = 0x10; // PCI bus 0, display device 2, function 0.
    cpu.flags = 2;
    const uint8_t bootstrap[] = {0x9a, 3, 0, 0, 0xc0, 0xb8, 3, 0, 0xcd, 0x10, 0xf4};
    for (unsigned i = 0; i < sizeof bootstrap; i++)
        cpu.write(0x600 + i, bootstrap[i], 1);
    auto end = cpu.instructions + 10000000;
    while (!cpu.halted && cpu.instructions < end)
        cpu.step();
    if (!cpu.halted)
        throw std::runtime_error("VGA ROM initialization did not return");
    cpu.halted = false;
}
} // namespace pc
