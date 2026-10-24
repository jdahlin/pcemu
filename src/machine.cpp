#include "machine.hpp"
#include "int15.hpp"
#include "pci32.hpp"
#include "profile.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <iostream>
namespace pc {
Machine::Machine(const std::string &hd, const std::string &fd, size_t ram) : cpu(ram) {
    if (!hd.empty())
        disk = std::make_unique<Disk>(hd);
    ata.disk = disk.get();
    if (!fd.empty())
        floppy = std::make_unique<Disk>(fd, true);
    bindDevices();
    initializeFirmware();
}
void Machine::bindDevices() {
    ata.disk = disk.get();
    cpu.videoRead = [this](uint32_t a) { return vga.read(a); };
    cpu.videoWrite = [this](uint32_t a, uint8_t v) { vga.write(a, v); };
    cpu.bios = [this](uint8_t n) { return bios(n); };
    cpu.input = [this](uint16_t p, int w) { return in(p, w); };
    cpu.output = [this](uint16_t p, uint32_t v, int w) { out(p, v, w); };
}
int64_t Machine::rtcTime() const {
    return int64_t(std::time(nullptr)) + rtcOffset;
}
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
    if (!d->transfer(cpu, false, 0, 1, 0x7c00))
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
void Machine::restart() {
    // Preserve RAM, ROM assets, disk backing, diagnostics and the monotonic host
    // instruction clock. Reset architectural state and all implemented devices.
    resetRequested = false;
    resets++;
    std::fill(std::begin(cpu.r), std::end(cpu.r), 0);
    for (auto &segment : cpu.s)
        segment = Segment{};
    std::fill(std::begin(cpu.cr), std::end(cpu.cr), 0);
    std::fill(std::begin(cpu.dr), std::end(cpu.dr), 0);
    cpu.dr[6] = 0xffff0ff0;
    cpu.dr[7] = 0x400;
    cpu.cr[0] = 0x10;
    cpu.flags = 2;
    cpu.ip = 0;
    cpu.gdtBase = cpu.idtBase = cpu.ldtBase = cpu.trBase = cpu.trLimit = 0;
    cpu.gdtLimit = cpu.ldtLimit = cpu.ldtSelector = cpu.trSelector = 0;
    cpu.idtLimit = 0x3ff;
    cpu.tr32 = cpu.halted = cpu.nativeFallback = false;
    cpu.nativeValue = 0;
    cpu.a20 = true;
    cpu.interruptBlockedThrough = 0;
    cpu.systemAccess = 0;
    cpu.flushTLB();
    ata = ATA(disk.get());
    pic[0] = pic[1] = PIC8259{};
    pit = PIT8254{};
    for (auto &counter : pit.counters)
        counter.loadedAt = cpu.instructions / 4;
    vga = VGA{};
    std::fill(std::begin(ports), std::end(ports), 0);
    std::fill(std::begin(picInit), std::end(picInit), 0);
    picBase[0] = 8;
    picBase[1] = 0x70;
    keyLine = mouseLine = keyboardAux = ataLine = cascadeLine = keyboardFull = false;
    mouse = PS2Mouse{};
    pci = PCI{};
    nic.reset();
    nicLine = false;
    biosKeyPrefix = biosPauseBytes = 0;
    timerPending = timerInService = keyboardInService = ataInService = false;
    keyboardData = keyboardControllerPending = keyboardParameter = keyboardLEDs = 0;
    keyboardCommandByte = 0x45;
    keyboardScanSet = 2;
    keyboardOutputPort = 3;
    keyboardEnabled = true;
    keyboardLastCommand = false;
    keyboardNextAt = 0;
    keyboardReplies.clear();
    keys.clear();
    scanCodes.clear();
    waitingKey = false;
    ticks = ioReads = cmosIndex = 0;
    videoMode = 3;
    columns = 80;
    rows = 25;
    cursor = activePage = 0;
    for (uint32_t a = 0; a < 0x500; a++)
        cpu.write(a, 0, 1);
    initializeFirmware();
    if (vgaOnReset)
        initializeVga();
    boot(bootFromFloppy);
    if (verbose)
        std::fprintf(stderr, "Firmware restart %llu\n", static_cast<unsigned long long>(resets));
}
void Machine::scroll(unsigned top, unsigned left, unsigned bottom, unsigned right, int lines,
                     uint8_t attr) {
    top = std::min(top, rows - 1);
    bottom = std::min(bottom, rows - 1);
    left = std::min(left, columns - 1);
    right = std::min(right, columns - 1);
    if (top > bottom || left > right)
        return;
    int n = std::abs(lines), height = bottom - top + 1;
    if (!n || n > height)
        n = height;
    for (int yy = 0; yy < height; yy++) {
        int y = lines < 0 ? bottom - yy : top + yy, src = lines < 0 ? y - n : y + n;
        for (unsigned x = left; x <= right; x++) {
            auto v = src >= int(top) && src <= int(bottom)
                         ? cpu.read(0xb8000 + (src * columns + x) * 2 + activePage * 4096, 2)
                         : (uint32_t(attr) << 8) | 32;
            cpu.write(0xb8000 + (y * columns + x) * 2 + activePage * 4096, v, 2);
        }
    }
}
void Machine::character(uint8_t c, uint8_t attr, bool tty) {
    if (tty) {
        if (c == 13)
            cursor -= cursor % columns;
        else if (c == 10)
            cursor += columns;
        else if (c == 8) {
            if (cursor)
                cursor--;
        } else if (c == 7) {
        } else {
            cpu.write(0xb8000 + activePage * 4096 + cursor * 2, c | (attr << 8), 2);
            cursor++;
        }
        if (cursor >= columns * rows) {
            scroll(0, 0, rows - 1, columns - 1, 1, attr);
            cursor -= columns;
        }
    } else
        cpu.write(0xb8000 + activePage * 4096 + cursor * 2, c | (attr << 8), 2);
    cpu.write(0x450 + activePage * 2, (cursor / columns) * 256 + cursor % columns, 2);
}
void Machine::key(uint16_t k) {
    if (!k)
        return;
    bool shift = (k & 255) >= 'A' && (k & 255) <= 'Z';
    if (!(k >> 8)) {
        const char *ascii = "1234567890-=qwertyuiop[]asdfghjkl;'`\\zxcvbnm,./";
        const uint8_t scans[] = {2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 16, 17, 18, 19,
                                 20, 21, 22, 23, 24, 25, 26, 27, 30, 31, 32, 33, 34, 35, 36, 37,
                                 38, 39, 40, 41, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53};
        auto ch = uint8_t(k);
        auto lower = ch >= 'A' && ch <= 'Z' ? ch + 32 : ch;
        const char *shifted = "!@#$%^&*()_+{}:\"~|<>?";
        const char *plain = "1234567890-=[];'`\\,./";
        if (auto special = std::strchr(shifted, ch)) {
            lower = plain[special - shifted];
            shift = true;
        }
        auto found = std::strchr(ascii, lower);
        if (found)
            k |= uint16_t(scans[found - ascii]) << 8;
        else if (ch == ' ')
            k |= 0x3900;
        else if (ch == 13)
            k |= 0x1c00;
        else if (ch == 9)
            k |= 0x0f00;
        else if (ch == 8)
            k |= 0x0e00;
        else if (ch == 27)
            k |= 0x0100;
    }
    // Host input represents a physical keyboard even while Windows temporarily
    // leaves protected mode. BIOS input is produced only by its IRQ handler.
    auto scan = k >> 8;
    if (scan) {
        if (shift)
            scanCodes.push_back(0x2a);
        scanCodes.push_back(scan);
        scanCodes.push_back(scan | 0x80);
        if (shift)
            scanCodes.push_back(0xaa);
    }
    waitingKey = false;
    cpu.halted = false;
}
void Machine::keyboardPoll() {
    // No output-buffer/IRQ work is possible until queued input is ready or
    // the mouse sample deadline arrives. Inspect live state so guest I/O,
    // host input and restored snapshots need no cached-deadline invalidation.
    if (!keyboardFull && !mouse.packetDue(cpu.instructions) &&
        (cpu.instructions < keyboardNextAt ||
         (keyboardReplies.empty() && mouse.replies.empty() && scanCodes.empty() &&
          mouse.packets.empty())))
        return;
    mouse.poll(cpu.instructions);
    // Backpressure the injected scan stream until the current IRQ service ends.
    // Command replies can still arrive while a handler is waiting for an ACK.
    if (!keyboardFull && cpu.instructions >= keyboardNextAt) {
        std::deque<uint8_t> *queue = nullptr;
        bool auxiliary = false;
        if (!keyboardReplies.empty())
            queue = &keyboardReplies;
        else if (!mouse.replies.empty()) {
            queue = &mouse.replies;
            auxiliary = true;
        } else if (keyboardEnabled && !keyboardParameter && !(keyboardCommandByte & 0x10) &&
                   !(pic[0].isr & 2) && (!(keyboardCommandByte & 1) || !(pic[0].mask & 2)) &&
                   !scanCodes.empty())
            queue = &scanCodes;
        else if (!(keyboardCommandByte & 0x20) && !(pic[1].isr & 0x10) &&
                 (!(keyboardCommandByte & 2) || !(pic[1].mask & 0x10)) && !mouse.packets.empty()) {
            queue = &mouse.packets;
            auxiliary = true;
        }
        if (queue) {
            keyboardData = queue->front();
            queue->pop_front();
            keyboardAux = auxiliary;
            keyboardFull = true;
        }
    }
    if (keyboardFull && keyboardAux && (keyboardCommandByte & 2) && !mouseLine) {
        pic[1].raise(4);
        mouseLine = true;
    } else if (keyboardFull && !keyboardAux && (keyboardCommandByte & 1) && !keyLine) {
        pic[0].raise(1);
        keyLine = true;
    }
}
void Machine::keyboardCommand(uint8_t value) {
    keyboardLastCommand = true;
    keyboardControllerPending = 0;
    // AT controller F0..FF pulse low the output bits whose command bits are
    // zero. Output bit 0 is RESET#, so every even pulse command resets the CPU.
    if ((value & 0xf0) == 0xf0 && !(value & 1)) {
        resetRequested = true;
        return;
    }
    switch (value) {
    case 0x20:
        keyboardReplies.push_back(keyboardCommandByte);
        break;
    case 0x60:
    case 0xd1:
    case 0xd2:
    case 0xd3:
    case 0xd4:
        keyboardControllerPending = value;
        break;
    case 0xaa:
        keyboardCommandByte |= 4;
        keyboardReplies.push_back(0x55);
        break;
    case 0xab:
        keyboardReplies.push_back(0);
        break;
    case 0xad:
        keyboardCommandByte |= 0x10;
        break;
    case 0xae:
        keyboardCommandByte &= ~0x10;
        break;
    case 0xa7:
        keyboardCommandByte |= 0x20;
        break;
    case 0xa8:
        keyboardCommandByte &= ~0x20;
        break;
    case 0xa9:
        keyboardReplies.push_back(0);
        break;
    case 0xc0:
        keyboardReplies.push_back(0x80);
        break;
    case 0xd0:
        keyboardReplies.push_back((keyboardOutputPort & ~2) | (cpu.a20 ? 2 : 0));
        break;
    default:
        break;
    }
}
void Machine::keyboardWrite(uint8_t value) {
    keyboardLastCommand = false;
    if (keyboardControllerPending) {
        auto command = keyboardControllerPending;
        keyboardControllerPending = 0;
        if (command == 0x60) {
            keyboardCommandByte = value;
            if (!(value & 1))
                keyLine = false;
            if (!(value & 2))
                mouseLine = false;
        } else if (command == 0xd1) {
            keyboardOutputPort = value;
            cpu.a20 = value & 2;
            if (!(value & 1))
                resetRequested = true;
        } else if (command == 0xd2)
            keyboardReplies.push_back(value);
        else if (command == 0xd3)
            mouse.replies.push_back(value);
        else if (command == 0xd4)
            mouse.command(value);
        return;
    }
    if (keyboardParameter) {
        auto parameter = keyboardParameter;
        keyboardParameter = 0;
        keyboardReplies.push_back(0xfa);
        if (parameter == 0xed)
            keyboardLEDs = value & 7;
        if (parameter == 0xf0) {
            if (!value)
                keyboardReplies.push_back(keyboardScanSet);
            else if (value <= 3)
                keyboardScanSet = value;
        }
        return;
    }
    switch (value) {
    case 0xed:
    case 0xf0:
    case 0xf3:
        keyboardParameter = value;
        keyboardReplies.push_back(0xfa);
        break;
    case 0xee:
        keyboardReplies.push_back(0xee);
        break;
    case 0xf2:
        keyboardReplies.insert(keyboardReplies.end(), {0xfa, 0xab, 0x83});
        break;
    case 0xf4:
        keyboardEnabled = true;
        keyboardReplies.push_back(0xfa);
        break;
    case 0xf5:
        keyboardEnabled = false;
        keyboardLEDs = 0;
        keyboardScanSet = 2;
        keyboardReplies.push_back(0xfa);
        break;
    case 0xf6:
        keyboardLEDs = 0;
        keyboardScanSet = 2;
        keyboardReplies.push_back(0xfa);
        break;
    case 0xff:
        keyboardLEDs = 0;
        keyboardScanSet = 2;
        keyboardEnabled = true;
        keyboardReplies.insert(keyboardReplies.end(), {0xfa, 0xaa});
        break;
    case 0xfe:
        keyboardReplies.push_back(keyboardData);
        break;
    default:
        keyboardReplies.push_back(value >= 0xf7 ? 0xfa : 0xfe);
        break;
    }
}
void Machine::poll() {
    profile::event(profile::DevicePoll);
    if (resetRequested) {
        restart();
        return;
    }
    if (pit.tick(cpu.instructions / 4))
        pic[0].raise(0);
    keyboardPoll();
    if (ata.irq && !(ata.control & 2) && !ataLine) {
        pic[1].raise(6);
        ataLine = true;
    }
    if (!ata.irq)
        ataLine = false;
    if (nic.irq() && !nicLine) pic[1].raise(2);
    nicLine = nic.irq();
    // Slave state cannot change until acknowledgement below. Usually there
    // is no unmasked request, so avoid repeated priority scans on every step.
    int slave = (pic[1].irr & ~pic[1].mask) ? pic[1].pending() : -1;
    if (slave >= 0 && !cascadeLine) {
        pic[0].raise(2);
        cascadeLine = true;
    }
    if (slave < 0)
        cascadeLine = false;
    timerPending = pic[0].irr & 1;
    timerInService = pic[0].isr & 1;
    keyboardInService = pic[0].isr & 2;
    ataInService = pic[1].isr & 0x40;
    if (!(cpu.flags & IF) ||
        (cpu.interruptBlockedThrough && cpu.instructions <= cpu.interruptBlockedThrough))
        return;
    if (!(pic[0].irr & ~pic[0].mask))
        return;
    int irq = pic[0].pending();
    if (irq < 0)
        return;
    pic[0].acknowledge(irq);
    uint8_t vector = pic[0].base + irq;
    if (irq == 2) {
        if (slave >= 0) {
            pic[1].acknowledge(slave);
            vector = pic[1].base + slave;
        } else
            vector = pic[1].base + 7;
        cascadeLine = false;
    }
    timerPending = pic[0].irr & 1;
    timerInService = pic[0].isr & 1;
    keyboardInService = pic[0].isr & 2;
    ataInService = pic[1].isr & 0x40;
    cpu.halted = false;
    cpu.interrupt(vector);
}
void Machine::timer() {
    pic[0].raise(0);
    poll();
}
uint32_t Machine::in(uint16_t p, int w) {
    if (p >= NE2000::base && p < NE2000::base + 32) return nic.in(p, w);
    if (p >= 0x40 && p <= 0x42)
        return pit.read(p - 0x40, cpu.instructions / 4);
    if ((p >= 0x1f0 && p <= 0x1f7) || p == 0x3f6) {
        auto value = ata.in(p, w);
        if (p == 0x1f7)
            ataLine = false;
        return value;
    }
    if (p == 0x20 || p == 0xa0)
        return pic[p == 0xa0].read();
    if (p == 0x21 || p == 0xa1)
        return pic[p == 0xa1].mask;
    (void)w;
    ioReads++;
    if (p == 0x3da) {
        vga.in(p);
        return (ioReads & 1) ? 9 : 0;
    }
    if (p >= 0x3b0 && p <= 0x3df)
        return vga.in(p);
    if (p >= 0xcf8 && p <= 0xcff)
        return pci.in(p, w);
    if (p == 0x1ce)
        return vga.dispiIndex;
    if (p == 0x1cf)
        return vga.dispiRead();
    if (p == 0x64) {
        keyboardPoll();
        return (keyboardFull ? 1 : 0) | (keyboardFull && keyboardAux ? 0x20 : 0) |
               (keyboardCommandByte & 4) | (keyboardLastCommand ? 8 : 0);
    }
    if (p == 0x60) {
        keyboardPoll();
        if (verbose)
            std::fprintf(stderr, "KBC read %02x full=%d at %04x:%08x count=%llu\n", keyboardData,
                         keyboardFull, cpu.s[CS].selector, cpu.ip,
                         static_cast<unsigned long long>(cpu.instructions));
        if (keyboardFull) {
            if (keyboardAux) {
                mouse.lastSent = keyboardData;
                mouseLine = false;
            } else
                keyLine = false;
            keyboardFull = false;
            // About one PS/2 byte at the current instruction-clock ratio.
            keyboardNextAt = cpu.instructions + 4000;
        }
        return keyboardData;
    }
    if (p == 0x71) {
        std::time_t now = rtcTime();
        auto t = *std::localtime(&now);
        auto bcd = [](int v) { return (v / 10) * 16 + v % 10; };
        switch (cmosIndex & 127) {
        case 0:
            return bcd(t.tm_sec);
        case 2:
            return bcd(t.tm_min);
        case 4:
            return bcd(t.tm_hour);
        case 7:
            return bcd(t.tm_mday);
        case 8:
            return bcd(t.tm_mon + 1);
        case 9:
            return bcd(t.tm_year % 100);
        case 0xa:
            return 0x26;
        case 0xb:
            return 2;
        case 0xd:
            return 0x80;
        case 0x10:
            return floppy ? 0x40 : 0;
        case 0x12:
            return disk ? 0xf0 : 0;
        case 0x14:
            return 0x20 | (floppy ? 1 : 0);
        case 0x15:
            return 128;
        case 0x16:
            return 2;
        case 0x17:
        case 0x30:
            return ((cpu.mem.size() - 1024 * 1024) / 1024) & 255;
        case 0x18:
        case 0x31:
            return ((cpu.mem.size() - 1024 * 1024) / 1024) >> 8;
        default:
            return 0;
        }
    }
    if (p == 0x61)
        return (ports[p] & 0xcf) | (pit.counters[1].output(cpu.instructions / 4) ? 16 : 0) |
               (pit.counters[2].output(cpu.instructions / 4) ? 32 : 0);
    if (p == 0x92)
        return cpu.a20 ? 2 : 0;
    return ports[p];
}
void Machine::out(uint16_t p, uint32_t v, int w) {
    if (p >= NE2000::base && p < NE2000::base + 32) {
        nic.out(p, v, w);
        if (!nic.irq()) nicLine = false;
        return;
    }
    if (p >= 0xcf8 && p <= 0xcff) {
        pci.out(p, v, w);
        return;
    }
    if (p == 0x1ce) {
        vga.dispiIndex = v;
        return;
    }
    if (p == 0x1cf) {
        vga.dispiWrite(v);
        return;
    }
    if (p >= 0x40 && p <= 0x43) {
        pit.write(p - 0x40, v, cpu.instructions / 4);
        return;
    }
    if (p == 0x61)
        pit.counters[2].setGate(v & 1, cpu.instructions / 4);
    if ((p >= 0x1f0 && p <= 0x1f7) || p == 0x3f6) {
        ata.out(p, v, w);
        return;
    }
    if (w > 1) {
        for (int i = 0; i < w; i++)
            out(p + i, v >> (i * 8), 1);
        return;
    }
    if (p == 0x64 || p == 0x60) {
        if (verbose)
            std::fprintf(stderr, "KBC OUT %04x=%02x at %s\n", p, unsigned(v),
                         cpu.location().c_str());
        ports[p] = v;
        if (p == 0x64)
            keyboardCommand(v);
        else
            keyboardWrite(v);
        return;
    }
    if (p >= 0x3b0 && p <= 0x3df)
        vga.out(p, v);
    ports[p] = v;
    if (p == 0x20 || p == 0xa0)
        pic[p == 0xa0].command(v);
    if (p == 0x21 || p == 0xa1)
        pic[p == 0xa1].data(v);
    if (p == 0x20 || p == 0xa0 || p == 0x21 || p == 0xa1) {
        for (unsigned k = 0; k < 2; k++) {
            ports[k ? 0xa1 : 0x21] = pic[k].mask;
            picBase[k] = pic[k].base;
        }
        timerPending = pic[0].irr & 1;
        timerInService = pic[0].isr & 1;
        keyboardInService = pic[0].isr & 2;
        ataInService = pic[1].isr & 0x40;
    }
    if (p == 0x70)
        cmosIndex = v;
    if (p == 0x92) {
        cpu.a20 = v & 2;
        if (v & 1)
            resetRequested = true;
    }
    if (p == 0xe9)
        std::cout << char(v) << std::flush;
}
bool Machine::bios(uint8_t n) {
    if (n == 0xfe && cpu.s[CS].selector == 0xf000 && cpu.ip == 2) {
        resetRequested = true;
        return true;
    }
    if (verbose && n == 0x21 &&
        (cpu.get(4, 1) == 0x4b || cpu.get(4, 1) == 0x3d || cpu.get(4, 1) == 0x3c)) {
        std::string name;
        auto ptr = cpu.address(DS, cpu.get(DX, 2));
        for (unsigned i = 0; i < 128; i++) {
            auto ch = cpu.read(ptr + i, 1);
            if (!ch)
                break;
            name += char(ch);
        }
        std::fprintf(stderr, "DOS AH=%02x file=%s at %s\n", cpu.get(4, 1), name.c_str(),
                     cpu.location().c_str());
    }

    if (n == 0xfe && cpu.s[CS].selector == 0xf000 &&
        ((cpu.ip >= 0x1002 && cpu.ip < 0x1400) || cpu.ip == 0x2002 || cpu.ip == 0x2025 ||
         cpu.ip == 0x2047 || cpu.ip == 0x20a2)) {
        uint8_t v = cpu.ip == 0x2002   ? 8
                    : cpu.ip == 0x2025 ? 9
                    : cpu.ip == 0x2047 ? 0x74
                    : cpu.ip == 0x20a2 ? 0x15
                                       : (cpu.ip - 0x1002) / 4;
        auto addr = cpu.address(SS, cpu.get(SP, cpu.s[SS].big ? 4 : 2) + (v == 9      ? 6
                                                                          : v == 0x74 ? 24
                                                                                      : 4));
        auto saved = cpu.read(addr, 2);
        // A caller may PUSHF, modify its working flags, then CALL FAR through a
        // saved BIOS vector. Return status belongs to that saved flags image;
        // comparing working flags before/after the service loses explicit
        // assignments that happened to match the incoming working value.
        cpu.flags = (cpu.flags & ~(CF | ZF)) | (saved & (CF | ZF));
        service(v, true);
        // Bootstrap does not return to this ROM stub or its caller's frame.
        if (v == 0x19)
            return true;
        cpu.write(addr, (saved & ~(CF | ZF)) | (cpu.flags & (CF | ZF)), 2);
        return true;
    }
    if (n == 0x74)
        return false; // IRQ12 must execute the far-callback ROM stub.
    // Honor guest hooks (DOS installs INT 21h, keyboard, disk, etc.).
    if (cpu.read(n * 4, 4) != (0xf0001000u + n * 4))
        return false;
    return service(n);
}
bool Machine::service(uint8_t n, bool guestStub) {
    auto ah = cpu.get(4, 1), al = cpu.get(AX, 1);
    auto &c = cpu;
    auto ok = [&]() {
        c.flags &= ~CF;
        c.set(4, 0, 1);
    };
    auto error = [&](uint8_t e) {
        c.flags |= CF;
        c.set(4, e, 1);
    };
    if (verbose && n != 0x10 && n != 0x16 && n != 0x1a && n != 8 && n != 0x28)
        std::fprintf(stderr, "BIOS %02x AX=%04x BX=%04x CX=%04x DX=%04x ES=%04x\n", n, c.get(AX, 2),
                     c.get(BX, 2), c.get(CX, 2), c.get(DX, 2), c.s[ES].selector);
    switch (n) {
    case 8:
        ticks++;
        cpu.write(0x46c, ticks, 4);
        if (!guestStub)
            out(0x20, 0x20, 1);
        if (cpu.read(0x1c * 4, 4) != 0xf0001070)
            cpu.interrupt(0x1c);
        break;
    case 9:
        // The firmware IRQ handler must complete the controller/PIC handshake,
        // including when a protected-mode keyboard driver chains to this vector.
        if ((guestStub && c.get(AX, 1)) || (!guestStub && keyboardFull)) {
            auto scan = guestStub ? c.get(AX, 1) : in(0x60, 1);
            if (biosPauseBytes) {
                --biosPauseBytes;
                if (!guestStub)
                    out(0x20, 0x20, 1);
                break;
            }
            if (scan == 0xe1) {
                biosPauseBytes = 5;
                if (!guestStub)
                    out(0x20, 0x20, 1);
                break;
            }
            if (scan == 0xe0) {
                biosKeyPrefix = scan;
                if (!guestStub)
                    out(0x20, 0x20, 1);
                break;
            }
            bool extended = biosKeyPrefix != 0;
            biosKeyPrefix = 0;
            if (extended && ((scan & 127) == 42 || (scan & 127) == 54)) {
                if (!guestStub)
                    out(0x20, 0x20, 1);
                break;
            }
            auto shifts = c.read(0x417, 1);
            unsigned code = scan & 127;
            uint8_t bit = code == 42 ? 2 : code == 54 ? 1 : code == 29 ? 4 : code == 56 ? 8 : 0;
            if (bit) {
                shifts = (scan & 128) ? shifts & ~bit : shifts | bit;
                c.write(0x417, shifts, 1);
            } else if (code == 58 && !(scan & 128))
                c.write(0x417, shifts ^ 64, 1);
            else if (!(scan & 128)) {
                static const char ascii[] = {
                    0,   27,  '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 8,
                    9,   'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', 13,  0,
                    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', 39,  '`', 0,   92,  'z',
                    'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,   '*', 0,   ' '};
                unsigned ch = code < sizeof(ascii) ? uint8_t(ascii[code]) : 0;
                if (ch >= 'a' && ch <= 'z' && bool(shifts & 3) != bool(shifts & 64))
                    ch -= 32;
                else if (shifts & 3) {
                    const char *plain = "1234567890-=[];'`\\,./";
                    const char *shifted = "!@#$%^&*()_+{}:\"~|<>?";
                    if (ch)
                        if (auto found = std::strchr(plain, ch))
                            ch = uint8_t(shifted[found - plain]);
                }
                if ((shifts & 4) && ch >= 'a' && ch <= 'z')
                    ch -= 96;
                keys.push_back((code << 8) | ch);
            }
        }
        if (!guestStub)
            out(0x20, 0x20, 1);
        break;
    case 0x74: {
        uint8_t byte = guestStub ? c.get(AX, 1) : in(0x60, 1);
        c.set(AX, 0, 1);
        if (biosMouseCount || (byte & 8)) {
            biosMousePacket[biosMouseCount++] = byte;
            if (biosMouseCount == 3) {
                biosMouseCount = 0;
                if (mouse.enabled && c.read(0xf0320, 4)) {
                    for (unsigned i = 0; i < 3; ++i)
                        c.write(0xf0324 + i * 2, biosMousePacket[i], 2);
                    c.set(AX, 1, 1);
                }
            }
        }
        if (!guestStub) {
            out(0xa0, 0x20, 1);
            out(0x20, 0x20, 1);
        }
        break;
    }
    case 0x1c:
    case 0x28:
    case 0x2a:
        break;
    case 0x10:
        switch (ah) {
        case 0:
            vga.mode(al & 127, !(al & 128));
            videoMode = al & 127;
            columns = videoMode == 0 || videoMode == 1 ? 40 : 80;
            rows = 25;
            cursor = 0;
            activePage = 0;
            c.write(0x449, videoMode, 1);
            c.write(0x44a, columns, 2);
            if (!(al & 128))
                for (unsigned i = 0; i < columns * rows; i++)
                    c.write(0xb8000 + i * 2, 0x0720, 2);
            break;
        case 1:
            c.write(0x460, c.get(CX, 2), 2);
            break;
        case 2: {
            unsigned page = c.get(7, 1) & 7;
            auto pos = c.get(DX, 2);
            c.write(0x450 + page * 2, pos, 2);
            if (page == activePage)
                cursor = (pos >> 8) * columns + (pos & 255);
            break;
        }
        case 3:
            c.set(DX, c.read(0x450 + (c.get(7, 1) & 7) * 2, 2), 2);
            c.set(CX, c.read(0x460, 2), 2);
            break;
        case 5:
            activePage = al & 7;
            c.write(0x462, activePage, 1);
            c.write(0x44e, activePage * 4096, 2);
            {
                auto pos = c.read(0x450 + activePage * 2, 2);
                cursor = (pos >> 8) * columns + (pos & 255);
            }
            break;
        case 6:
        case 7:
            scroll(c.get(5, 1), c.get(CX, 1), c.get(6, 1), c.get(DX, 1),
                   ah == 7 ? -int(al) : int(al), c.get(7, 1));
            break;
        case 8:
            c.set(AX, c.read(0xb8000 + (c.get(7, 1) & 7) * 4096 + cursor * 2, 2), 2);
            break;
        case 9:
        case 0xa: {
            auto addr = 0xb8000 + (c.get(7, 1) & 7) * 4096 + cursor * 2;
            for (unsigned i = 0; i < c.get(CX, 2) && i < columns * rows; i++) {
                c.write(addr + i * 2, al, 1);
                if (ah == 9)
                    c.write(addr + i * 2 + 1, c.get(BX, 1), 1);
            }
            break;
        }
        case 0xe:
            character(al, 7);
            break;
        case 0xf:
            c.set(AX, (columns << 8) | videoMode, 2);
            c.set(7, activePage, 1);
            break;
        case 0x10:
            if (al == 0) {
                vga.attr[c.get(BX, 1) & 15] = c.get(7, 1);
            } else if (al == 1) {
                vga.attr[0x11] = c.get(7, 1);
            } else if (al == 2 || al == 9) {
                auto ptr = c.address(ES, c.get(DX, 2));
                for (unsigned i = 0; i < 16; i++) {
                    if (al == 2)
                        vga.attr[i] = c.read(ptr + i, 1);
                    else
                        c.write(ptr + i, vga.attr[i], 1);
                }
                if (al == 2)
                    vga.attr[0x11] = c.read(ptr + 16, 1);
                else
                    c.write(ptr + 16, vga.attr[0x11], 1);
            } else if (al == 3) {
                vga.attr[0x10] = (vga.attr[0x10] & ~8) | (c.get(BX, 1) ? 8 : 0);
            } else if (al == 7) {
                c.set(7, vga.attr[c.get(BX, 1) & 15], 1);
            } else if (al == 8) {
                c.set(7, vga.attr[0x11], 1);
            } else if (al == 0x10) {
                unsigned idx = c.get(BX, 2) & 255;
                vga.dac[idx][0] = c.get(6, 1) & 63;
                vga.dac[idx][1] = c.get(5, 1) & 63;
                vga.dac[idx][2] = c.get(CX, 1) & 63;
            } else if (al == 0x12) {
                unsigned idx = c.get(BX, 2) & 255;
                auto ptr = c.address(ES, c.get(DX, 2));
                for (unsigned i = 0; i < c.get(CX, 2) && idx + i < 256; i++)
                    for (unsigned j = 0; j < 3; j++)
                        vga.dac[idx + i][j] = c.read(ptr + i * 3 + j, 1) & 63;
            }
            break;
        case 0x11:
            if (al == 0x30) {
                c.segment(ES, 0xc000);
                c.set(BP, 0x8ee8, 2);
                c.set(CX, 8, 2);
                c.set(DX, 24, 2);
            }
            break;
        case 0x12:
            if (c.get(BX, 1) == 0x10) {
                c.set(BX, 3, 2);
                c.set(CX, 9, 2);
            }
            break;
        case 0x1a:

            c.set(AX, 0x1a, 2);
            c.set(BX, 8, 2);
            break;
        case 0x1b:
            c.set(AX, 0, 2);
            break;
        default:
            break;
        }
        break;
    case 0x11:
        c.set(AX, c.read(0x410, 2), 2);
        break;
    case 0x12:
        c.set(AX, 640, 2);
        break;
    case 0x13: {
        auto drive = c.get(DX, 1);
        Disk *d = drive == 0x80 ? disk.get() : drive == 0 ? floppy.get() : nullptr;
        unsigned statusAddress = drive & 0x80 ? 0x474 : 0x441;
        auto diskOK = [&]() {
            ok();
            c.write(statusAddress, 0, 1);
        };
        auto diskError = [&](uint8_t code) {
            error(code);
            c.write(statusAddress, code, 1);
        };
        if (ah == 0) {
            diskOK();
            break;
        }
        if (!d) {
            diskError(0x80);
            break;
        }
        if (ah == 1) {
            c.set(4, c.read(statusAddress, 1), 1);
            c.flags = (c.flags & ~CF) | (c.get(4, 1) ? CF : 0);
            break;
        }
        if (ah == 2 || ah == 3 || ah == 4) {
            unsigned sector = c.get(CX, 1) & 63, cyl = c.get(5, 1) | ((c.get(CX, 1) & 0xc0) << 2),
                     head = c.get(6, 1), count = al;
            uint64_t lba = (uint64_t(cyl) * d->heads + head) * d->spt + sector - 1;
            if (sector < 1 || sector > d->spt || head >= d->heads || !count) {
                diskError(4);
                break;
            }
            if (ah == 4 || d->transfer(c, ah == 3, lba, count, c.address(ES, c.get(BX, 2)))) {
                diskOK();
                c.set(AX, count, 1);
            } else
                diskError(4);
            break;
        }
        if (ah == 8) {
            unsigned cyl = std::min<uint64_t>(1024, d->sectors / (d->heads * d->spt));
            if (cyl)
                cyl--;
            c.set(CX, ((cyl & 255) << 8) | ((cyl >> 2) & 0xc0) | d->spt, 2);
            c.set(DX, ((d->heads - 1) << 8) | 1, 2);
            c.set(BX, d->floppy ? 4 : 0, 1);
            if (d->floppy) {
                c.segment(ES, 0xf000);
                c.set(DI, 0x400, 2);
            }
            diskOK();
            break;
        }
        if (ah == 0x15) {
            c.flags &= ~CF;
            c.set(4, d->floppy ? 1 : 3, 1);
            if (!d->floppy) {
                c.set(CX, d->sectors >> 16, 2);
                c.set(DX, d->sectors, 2);
            }
            break;
        }
        if (ah == 0x41 && c.get(BX, 2) == 0x55aa) {
            c.set(BX, 0xaa55, 2);
            c.set(CX, 1, 2);
            c.set(4, 0x30, 1);
            c.flags &= ~CF;
            break;
        }
        if (ah == 0x42 || ah == 0x43) {
            auto ptr = c.address(DS, c.get(SI, 2));
            unsigned count = c.read(ptr + 2, 2);
            uint64_t lba = uint64_t(c.read(ptr + 12, 4)) << 32 | c.read(ptr + 8, 4);
            auto dest = (c.read(ptr + 6, 2) << 4) + c.read(ptr + 4, 2);
            if (c.read(ptr, 1) >= 16 && d->transfer(c, ah == 0x43, lba, count, dest))
                diskOK();
            else
                diskError(4);
            break;
        }
        if (ah == 0x48) {
            auto ptr = c.address(DS, c.get(SI, 2));
            if (c.read(ptr, 2) < 26) {
                diskError(1);
                break;
            }
            c.write(ptr, 26, 2);
            c.write(ptr + 2, 2, 2);
            c.write(ptr + 4, d->sectors / (d->heads * d->spt), 4);
            c.write(ptr + 8, d->heads, 4);
            c.write(ptr + 12, d->spt, 4);
            c.write(ptr + 16, d->sectors, 4);
            c.write(ptr + 20, d->sectors >> 32, 4);
            c.write(ptr + 24, 512, 2);
            diskOK();
            break;
        }
        if (ah == 0x18 || ah == 0x17) {
            diskOK();
            break;
        }
        diskError(1);
        break;
    }
    case 0x14:
        c.set(AX, 0x8000, 2);
        break;
    case 0x15:
        c.write(0xf032a, 0, 1);
        switch (ah) {
        case 0x24:
            c.a20 = al == 1 ? true : al == 0 ? false : c.a20;
            ok();
            if (al == 2)
                c.set(AX, c.a20 ? 1 : 0, 1);
            if (al == 3)
                c.set(BX, 3, 2);
            break;
        case 0x4f:
            c.flags |= CF;
            break;
        case 0x83:
        case 0x86:
        case 0x90:
        case 0x91:
            ok();
            break;
        case 0x87: {
            auto ptr = c.address(ES, c.get(SI, 2));
            auto descbase = [&](unsigned off) {
                auto lo = c.read(ptr + off, 4), hi = c.read(ptr + off + 4, 4);
                return (lo >> 16) | ((hi & 255) << 16) | (hi & 0xff000000);
            };
            auto src = descbase(0x10), dst = descbase(0x18);
            for (unsigned i = 0; i < c.get(CX, 2) * 2; i++)
                c.write(dst + i, c.read(src + i, 1), 1);
            ok();
            break;
        }
        case 0x88:
            c.set(AX, (c.mem.size() - 1024 * 1024) / 1024, 2);
            c.flags &= ~CF;
            break;
        case 0xc0:
            c.segment(ES, 0xf000);
            c.set(BX, 0x300, 2);
            ok();
            break;
        case 0xc1:
            error(0x86);
            break;
        case 0xc2: {
            auto bh = c.get(BX, 2) >> 8;
            switch (al) {
            case 0:
                if (bh > 1) {
                    error(2);
                    break;
                }
                if (bh && !c.read(0xf0320, 4)) {
                    error(5);
                    break;
                }
                c.write(0xf032a, bh ? 1 : 2, 1);
                mouse.enabled = bh;
                keyboardCommandByte = (keyboardCommandByte & ~0x20) | 2;
                if (bh) {
                    pic[0].mask &= ~4;
                    pic[1].mask &= ~0x10;
                } else {
                    mouse.packets.clear();
                    biosMouseCount = 0;
                }
                ok();
                break;
            case 1:
                mouse.defaults();
                biosMouseCount = 0;
                c.set(BX, 0xaa, 2);
                ok();
                break;
            case 2: {
                const uint8_t rates[] = {10, 20, 40, 60, 80, 100, 200};
                if (bh >= 7) {
                    error(2);
                    break;
                }
                mouse.rate = rates[bh];
                ok();
                break;
            }
            case 3:
                if (bh > 3) {
                    error(2);
                    break;
                }
                mouse.resolution = bh;
                ok();
                break;
            case 4:
                c.set(BX, (c.get(BX, 2) & 255) | (mouse.id << 8), 2);
                ok();
                break;
            case 5:
                if (bh != 3) {
                    error(2);
                    break;
                }
                mouse.defaults();
                biosMouseCount = 0;
                ok();
                break;
            case 6:
                if (bh == 0) {
                    c.set(BX, (c.get(BX, 2) & 0xff00) | mouse.status(), 2);
                    c.set(CX, (c.get(CX, 2) & 0xff00) | mouse.resolution, 2);
                    c.set(DX, (c.get(DX, 2) & 0xff00) | mouse.rate, 2);
                } else if (bh <= 2)
                    mouse.scaling2 = bh == 2;
                else {
                    error(2);
                    break;
                }
                ok();
                break;
            case 7:
                c.write(0xf0320, (uint32_t(c.s[ES].selector) << 16) | c.get(BX, 2), 4);
                biosMouseCount = 0;
                if (!c.read(0xf0320, 4)) {
                    mouse.enabled = false;
                    mouse.packets.clear();
                }
                ok();
                break;
            default:
                error(1);
                break;
            }
            break;
        }
        default:
            error(0x86);
            break;
        }
        break;
    case 0x16:
        if (ah == 0 || ah == 0x10) {
            if (keys.empty()) {
                waitingKey = true;
                c.halted = true;
                // The firmware waits with interrupts enabled so IRQ1 can fill
                // its input buffer. Caller flags remain in the INT frame.
                c.flags |= IF;
                c.ip = (c.ip - 2) & 65535;
            } else {
                c.set(AX, keys.front(), 2);
                keys.pop_front();
                waitingKey = false;
            }
        } else if (ah == 1 || ah == 0x11) {
            if (keys.empty())
                c.flags |= ZF;
            else {
                c.flags &= ~ZF;
                c.set(AX, keys.front(), 2);
            }
        } else if (ah == 2)
            c.set(AX, c.read(0x417, 1), 1);
        else if (ah == 5) {
            keys.push_back(c.get(CX, 2));
            c.set(AX, 0, 1);
        } else if (ah == 0x12)
            c.set(AX, 0, 2);
        break;
    case 0x17:
        c.set(4, 0x90, 1);
        break;
    case 0x18:
        throw std::runtime_error("BIOS boot failed (INT 18h)");
    case 0x19:
        boot(bootFromFloppy);
        break;
    case 0x1a:
        if (ah == 0xb1) {
            auto bdf = c.get(BX, 2), offset = c.get(DI, 2);
            auto failure = [&](uint8_t status) {
                c.set(4, status, 1);
                c.flags |= CF;
            };
            switch (al) {
            case 1:
                c.set(AX, 1, 2);
                c.set(BX, 0x0210, 2);
                c.set(CX, 0, 2);
                c.r[DX] = 0x20494350;
                c.r[DI] = 0;
                c.flags &= ~CF;
                break;
            case 2:
            case 3: {
                bool found = al == 2 ? c.get(DX, 2) == 0x1234 && c.get(CX, 2) == 0x1111
                                     : (c.r[CX] & 0xffffff) == 0x030000;
                if (!found || c.get(SI, 2))
                    failure(0x86);
                else {
                    c.set(BX, 0x10, 2);
                    ok();
                }
                break;
            }
            case 8:
            case 9:
            case 10:
            case 11:
            case 12:
            case 13: {
                unsigned width = 1u << ((al - 8) % 3);
                if (offset + width > 256 || (offset & (width - 1))) {
                    failure(0x87);
                    break;
                }
                if (al <= 10)
                    c.set(CX, pci.read(bdf, offset, width), width);
                else
                    pci.write(bdf, offset, c.get(CX, width), width);
                ok();
                break;
            }
            default:
                failure(0x81);
                break;
            }
            break;
        }
        if (ah == 0) {
            c.set(CX, ticks >> 16, 2);
            c.set(DX, ticks, 2);
            c.set(AX, 0, 1);
            c.flags &= ~CF;
        } else if (ah == 2 || ah == 4) {
            std::time_t now = rtcTime();
            auto t = *std::localtime(&now);
            auto bcd = [](int v) { return (v / 10) * 16 + v % 10; };
            c.set(CX,
                  ah == 2 ? (bcd(t.tm_hour) << 8) | bcd(t.tm_min) : 0x2000 | bcd(t.tm_year % 100),
                  2);
            c.set(DX, ah == 2 ? bcd(t.tm_sec) << 8 : (bcd(t.tm_mon + 1) << 8) | bcd(t.tm_mday), 2);
            c.flags &= ~CF;
        } else
            error(0x86);
        break;
    case 0x2f:
        c.set(AX, 0, 1);
        break;
    case 0x33:
        c.set(AX, 0, 2);
        c.set(BX, 0, 2);
        break;
    default:
        if (n < 8)
            throw std::runtime_error("unhandled CPU exception " + std::to_string(n) + " at " +
                                     c.location());
        break;
    }
    return true;
}
std::string Machine::text() const {
    std::string result;
    for (unsigned y = 0; y < rows; y++) {
        std::string line;
        for (unsigned x = 0; x < columns; x++) {
            auto ch = cpu.mem[0xb8000 + activePage * 4096 + (y * columns + x) * 2];
            line += ch >= 32 && ch < 127 ? char(ch) : ' ';
        }
        while (!line.empty() && line.back() == ' ')
            line.pop_back();
        result += line + '\n';
    }
    return result;
}
} // namespace pc
