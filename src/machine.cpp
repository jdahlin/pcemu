#include "machine.hpp"
#include "profile.hpp"
#include <algorithm>
#include <cstdio>
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
    rtc.configure({cpu.mem.size(), bool(disk), bool(floppy)});
    bindIoPorts();
    cpu.videoRead = [this](uint32_t a) { return vga.read(a); };
    cpu.videoWrite = [this](uint32_t a, uint8_t v) { vga.write(a, v); };
    cpu.bios = [this](uint8_t n) { return bios(n); };
    cpu.input = [this](uint16_t p, int w) { return in(p, w); };
    cpu.output = [this](uint16_t p, uint32_t v, int w) { out(p, v, w); };
}
int64_t Machine::rtcTime() const {
    return rtc.now();
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
    ticks = ioReads = 0;
    rtc.reset();
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
    if (auto value = ioPorts.read(p, w)) return *value;
    ++ioReads;
    return ports[p];
}
void Machine::out(uint16_t p, uint32_t v, int w) {
    if (ioPorts.write(p, v, w)) return;
    if (w > 1) {
        for (int i = 0; i < w; ++i) out(p + i, v >> (8 * i), 1);
        return;
    }
    ports[p] = v;
}
} // namespace pc
