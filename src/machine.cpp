#include "machine.hpp"
#include "profile.hpp"
#include "pc_input.hpp"
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
    ata.reset(disk.get());
    pic.reset();
    pit.reset(cpu.instructions / 4);
    vga.reset();
    std::fill(std::begin(ports), std::end(ports), 0);
    ataLine = false;
    systemPortB.reset();
    debugConsole.reset();
    ps2.reset();
    pci.reset();
    nic.reset();
    nicLine = false;
    biosKeyPrefix = biosPauseBytes = 0;
    keys.clear();
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
void Machine::key(uint16_t value) {
    if (!value) return;
    enqueuePcKey(ps2, value);
    waitingKey = false;
    cpu.halted = false;
}
I8042Device::IrqState Machine::ps2IrqState() const {
    return {bool(pic[0].isr & 2), bool(pic[0].mask & 2),
            bool(pic[1].isr & 0x10), bool(pic[1].mask & 0x10)};
}
void Machine::applyPs2Irqs(I8042Device::Irqs irqs) {
    if (irqs.keyboard) pic[0].raise(1);
    if (irqs.mouse) pic[1].raise(4);
}
void Machine::pollPs2() {
    applyPs2Irqs(ps2.poll(cpu.instructions, ps2IrqState()));
}
void Machine::poll() {
    profile::event(profile::DevicePoll);
    if (resetRequested) {
        restart();
        return;
    }
    if (pit.tick(cpu.instructions / 4))
        pic[0].raise(0);
    pollPs2();
    if (ata.interruptLevel() && !ataLine) {
        pic[1].raise(6);
        ataLine = true;
    }
    if (!ata.irq)
        ataLine = false;
    if (nic.irq() && !nicLine) pic[1].raise(2);
    nicLine = nic.irq();
    const bool ready = (cpu.flags & IF) &&
        !(cpu.interruptBlockedThrough && cpu.instructions <= cpu.interruptBlockedThrough);
    if (auto vector = pic.nextVector(ready)) {
        cpu.halted = false;
        cpu.interrupt(*vector);
    }
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
