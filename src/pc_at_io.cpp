#include "machine.hpp"
#include <cstdio>
#include <iostream>

namespace pc {
void Machine::bindIoPorts() {
    ioPorts.clear();
    auto ataRead = [this](uint16_t p, int w) {
        auto value = ata.in(p, w);
        if (p == 0x1f7)
            ataLine = false;
        return value;
    };
    auto ataWrite = [this](uint16_t p, uint32_t v, int w) { ata.out(p, v, w); };
    ioPorts.map("ata-primary", 0x1f0, 8, ataRead, ataWrite);
    ioPorts.map("ata-control", 0x3f6, 1, ataRead, ataWrite);
    ioPorts.map("ne2000", NE2000::base, 32,
                [this](uint16_t p, int w) { return nic.in(p, w); },
                [this](uint16_t p, uint32_t v, int w) {
                    nic.out(p, v, w);
                    if (!nic.irq()) nicLine = false;
                });

    // These reads historically advanced the retrace counter; ATA, NIC, PIC and
    // PIT data reads did not. Keep that observable ordering during extraction.
    auto counted = [this](IoPortSpace::Reader read) -> IoPortSpace::Reader {
        return [this, read](uint16_t p, int w) { ++ioReads; return read(p, w); };
    };
    auto saved = [this](uint16_t p, int) -> uint32_t { return ports[p]; };
    auto store = [this](uint16_t p, uint32_t v, int) { ports[p] = v; };
    // Byte-oriented devices split wide writes through the bus, so the next
    // byte can belong to another region. Reads still remain one transaction.
    auto bytes = [this](IoPortSpace::Writer write) -> IoPortSpace::Writer {
        return [this, write](uint16_t p, uint32_t v, int w) {
            if (w > 1) {
                for (int i = 0; i < w; ++i) out(p + i, v >> (8 * i), 1);
            } else write(p, v, w);
        };
    };
    for (uint16_t base : {0x20, 0xa0})
        ioPorts.map(base == 0x20 ? "pic-master" : "pic-slave", base, 2,
            [this](uint16_t p, int) -> uint32_t {
                auto &controller = pic[p >= 0xa0];
                return (p & 1) ? controller.mask : controller.read();
            }, bytes([this](uint16_t p, uint32_t v, int) { writePicPort(p, v); }));
    ioPorts.map("pit", 0x40, 4,
        [this](uint16_t p, int) -> uint32_t {
            if (p == 0x43) { ++ioReads; return ports[p]; }
            return pit.read(p - 0x40, cpu.instructions / 4);
        }, [this](uint16_t p, uint32_t v, int) { pit.write(p - 0x40, v, cpu.instructions / 4); });
    ioPorts.map("pci-config", 0xcf8, 8,
        counted([this](uint16_t p, int w) { return pci.in(p, w); }),
        [this](uint16_t p, uint32_t v, int w) { pci.out(p, v, w); });
    ioPorts.map("dispi", 0x1ce, 2,
        counted([this](uint16_t p, int) -> uint32_t {
            return p == 0x1ce ? vga.dispiIndex : vga.dispiRead();
        }), [this](uint16_t p, uint32_t v, int) {
            if (p == 0x1ce) vga.dispiIndex = v;
            else vga.dispiWrite(v);
        });
    ioPorts.map("vga", 0x3b0, 0x30,
        counted([this](uint16_t p, int) -> uint32_t {
            auto value = vga.in(p);
            return p == 0x3da ? ((ioReads & 1) ? 9 : 0) : value;
        }), bytes([this](uint16_t p, uint32_t v, int) { vga.out(p, v); ports[p] = v; }));
    auto keyboardWritePort = bytes([this](uint16_t p, uint32_t v, int) {
        if (verbose)
            std::fprintf(stderr, "KBC OUT %04x=%02x at %s\n", p, unsigned(v), cpu.location().c_str());
        ports[p] = v;
        auto effects = p == 0x64 ? ps2.command(v, cpu.a20) : ps2.write(v);
        if (effects.a20) cpu.a20 = *effects.a20;
        resetRequested |= effects.reset;
    });
    for (uint16_t p : {0x60, 0x64})
        ioPorts.map(p == 0x60 ? "ps2-data" : "ps2-command", p, 1,
            counted([this](uint16_t p, int) -> uint32_t {
                auto read = p == 0x64 ? ps2.readStatus(cpu.instructions, ps2IrqState())
                                      : ps2.readData(cpu.instructions, ps2IrqState());
                applyPs2Irqs(read.irqs);
                if (p == 0x60 && verbose)
                    std::fprintf(stderr, "KBC read %02x full=%d at %04x:%08x count=%llu\n", read.value,
                                 read.full, cpu.s[CS].selector, cpu.ip,
                                 static_cast<unsigned long long>(cpu.instructions));
                return read.value;
            }), keyboardWritePort);
    auto speakerWrite = bytes(store);
    ioPorts.map("speaker", 0x61, 1,
        counted([this](uint16_t p, int) -> uint32_t {
            return (ports[p] & 0xcf) | (pit.counters[1].output(cpu.instructions / 4) ? 16 : 0) |
                   (pit.counters[2].output(cpu.instructions / 4) ? 32 : 0);
        }), [this, speakerWrite](uint16_t p, uint32_t v, int w) {
            // The gate was updated before splitting a wide write, then again
            // for its low byte. Preserve this until timing is refactored.
            pit.counters[2].setGate(v & 1, cpu.instructions / 4);
            speakerWrite(p, v, w);
        });
    ioPorts.map("cmos", 0x70, 2,
        counted([this](uint16_t p, int) -> uint32_t { return p == 0x71 ? rtc.readData() : ports[p]; }),
        bytes([this](uint16_t p, uint32_t v, int) {
            ports[p] = v;
            if (p == 0x70) rtc.select(v);
        }));
    ioPorts.map("system-control", 0x92, 1,
        counted([this](uint16_t, int) -> uint32_t { return cpu.a20 ? 2 : 0; }),
        bytes([this](uint16_t p, uint32_t v, int) {
            ports[p] = v;
            cpu.a20 = v & 2;
            if (v & 1) resetRequested = true;
        }));
    ioPorts.map("debug-console", 0xe9, 1, counted(saved),
        bytes([this](uint16_t p, uint32_t v, int) {
            ports[p] = v;
            std::cout << char(v) << std::flush;
        }));
}

void Machine::writePicPort(uint16_t p, uint32_t v) {
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
}
} // namespace pc
