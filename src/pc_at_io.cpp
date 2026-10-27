#include "machine.hpp"
#include <cstdio>
#include <iostream>

namespace pc {
void Machine::bindIoPorts() {
    ioPorts.clear();
    systemPortB.restore(ports[0x61]);
    debugConsole.restore(ports[0xe9]);
    debugConsole.bind_output_sink([](uint8_t value) { std::cout << char(value) << std::flush; });
    ioPorts.map("absolute-mouse", AbsoluteMouse::port, 1,
        [this](uint16_t, int width) -> uint32_t {
            return absoluteMouse.read(width, cpu.r) ? cpu.r[AX] : 0xffffffffu;
        }, [](uint16_t, uint32_t, int) {});
    ioPorts.map("host-cursor", 0x5659, 2,
        [](uint16_t, int) -> uint32_t { return 0xc5a1; },
        [this](uint16_t p, uint32_t v, int w) {
            auto revision = hostCursor.revision;
            hostCursor.write(p, v, w);
            if (revision != hostCursor.revision) {
                auto expand = [this](unsigned c) { return (vga.dispi[4] & 0x20) ? c : (c << 2) | (c >> 4); };
                for (unsigned i = 0; i < 256; ++i)
                    hostCursor.palette[i] = expand(vga.dac[i][0]) << 16 |
                        expand(vga.dac[i][1]) << 8 | expand(vga.dac[i][2]);
            }
        });
    auto ataRead = [this](uint16_t p, int w) {
        auto value = ata.read_register(p, w);
        if (p == 0x1f7)
            ataLine = false;
        return value;
    };
    auto ataWrite = [this](uint16_t p, uint32_t v, int w) { ata.write_register(p, v, w); };
    ioPorts.map("ata-primary", 0x1f0, 8, ataRead, ataWrite);
    ioPorts.map("ata-control", 0x3f6, 1, ataRead, ataWrite);
    ioPorts.map("ne2000", NE2000::base, 32,
                [this](uint16_t p, int w) { return nic.read_register(p, w); },
                [this](uint16_t p, uint32_t v, int w) {
                    nic.write_register(p, v, w);
                    if (!nic.interrupt_level()) nicLine = false;
                });

    // These reads historically advanced the retrace counter; ATA, NIC, PIC and
    // PIT data reads did not. Keep that observable ordering during extraction.
    auto counted = [this](IoPortSpace::Reader read) -> IoPortSpace::Reader {
        return [this, read](uint16_t p, int w) { ++ioReads; return read(p, w); };
    };
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
    ioPorts.map("dma-primary", 0x00, 0x10,
        [this](uint16_t p, int) -> uint32_t { pollSoundBlaster(); return dma.read_register(p); },
        bytes([this](uint16_t p, uint32_t v, int) { pollSoundBlaster(); dma.write_register(p,v); }));
    // PC/AT external page latches: channels 0,1,2,3 use 87h,83h,81h,82h.
    // Other page ports retain their existing board latch behavior.
    // IBM PC/AT Technical Reference, System Board I/O Address Map.
    ioPorts.map("dma-pages", 0x80, 0x10,
        [this](uint16_t p, int) -> uint32_t { return ports[p]; },
        bytes([this](uint16_t p, uint32_t v, int) {
            pollSoundBlaster();
            ports[p]=v;
            int channel=p==0x87?0:p==0x83?1:p==0x81?2:p==0x82?3:-1;
            if(channel>=0)dma.state.channels[channel].page=v;
        }));
    ioPorts.map("sound-blaster",SoundBlasterDevice::BasePort,16,
        [this](uint16_t p,int)->uint32_t {
            pollSoundBlaster();
            auto value=soundBlaster.read_register(p-SoundBlasterDevice::BasePort);
            soundBlasterLine=soundBlaster.state.irq_pending;
            return value;
        },bytes([this](uint16_t p,uint32_t value,int) {
            pollSoundBlaster();
            soundBlaster.write_register(p-SoundBlasterDevice::BasePort,value,pitTicks());
            pollSoundBlaster();
        }));
    for (uint16_t base : {0x20, 0xa0})
        ioPorts.map(base == 0x20 ? "pic-master" : "pic-slave", base, 2,
            [this](uint16_t p, int) -> uint32_t {
                return pic.read_register(p >= 0xa0, p & 1);
            }, bytes([this](uint16_t p, uint32_t v, int) {
                ports[p] = v;
                pic.write_register(p >= 0xa0, p & 1, v);
                for (unsigned i = 0; i < 2; ++i) ports[i ? 0xa1 : 0x21] = pic[i].mask;
            }));
    ioPorts.map("pit", 0x40, 4,
        [this](uint16_t p, int) -> uint32_t {
            if (p == 0x43) { ++ioReads; return ports[p]; }
            return pit.read_counter(p - 0x40, pitTicks());
        }, [this](uint16_t p, uint32_t v, int) { pit.write_register(p - 0x40, v, pitTicks()); });
    ioPorts.map("pci-config", 0xcf8, 8,
        counted([this](uint16_t p, int w) { return pci.read_io_register(p, w); }),
        [this](uint16_t p, uint32_t v, int w) { pci.write_io_register(p, v, w); });
    ioPorts.map("dispi", 0x1ce, 2,
        counted([this](uint16_t p, int) -> uint32_t {
            return vga.read_display_interface_port(p == 0x1cf);
        }), [this](uint16_t p, uint32_t v, int) {
            vga.advance_scanout(pitTicks());
            vga.write_display_interface_port(p == 0x1cf, v);
        });
    ioPorts.map("vga", 0x3b0, 0x30,
        counted([this](uint16_t p, int) -> uint32_t {
            return vga.read_io_register(p, pitTicks());
        }), bytes([this](uint16_t p, uint32_t v, int) { vga.advance_scanout(pitTicks()); vga.write_register(p, v); ports[p] = v; }));
    auto keyboardWritePort = bytes([this](uint16_t p, uint32_t v, int) {
        if (verbose)
            std::fprintf(stderr, "KBC OUT %04x=%02x at %s\n", p, unsigned(v), cpu.location().c_str());
        ports[p] = v;
        auto effects = p == I8042Device::Register::CommandRegister ? ps2.write_command_register(v, cpu.a20) : ps2.write_data_register(v);
        if (effects.a20) cpu.a20 = *effects.a20;
        resetRequested |= effects.reset;
    });
    for (uint16_t p : {0x60, 0x64})
        ioPorts.map(p == 0x60 ? "ps2-data" : "ps2-command", p, 1,
            counted([this](uint16_t p, int) -> uint32_t {
                auto read = p == I8042Device::Register::CommandRegister ? ps2.read_status_register(cpu.instructions, ps2IrqState())
                                      : ps2.read_data_register(cpu.instructions, ps2IrqState());
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
            (void)p;
            const auto now = pitTicks();
            return systemPortB.read_register(pit.counters[1].output_high(now),
                                    pit.counters[2].output_high(now));
        }), [this, speakerWrite](uint16_t p, uint32_t v, int w) {
            // The gate was updated before splitting a wide write, then again
            // for its low byte. Preserve this until timing is refactored.
            pit.counters[2].set_gate(systemPortB.write_register(v), pitTicks());
            speakerWrite(p, v, w);
        });
    ioPorts.map("cmos", 0x70, 2,
        counted([this](uint16_t p, int) -> uint32_t { return p == 0x71 ? rtc.read_data_register() : ports[p]; }),
        bytes([this](uint16_t p, uint32_t v, int) {
            ports[p] = v;
            if (p == 0x70) rtc.select_register(v);
        }));
    ioPorts.map("system-control", 0x92, 1,
        counted([this](uint16_t, int) -> uint32_t { return port92.read_register(cpu.a20); }),
        bytes([this](uint16_t p, uint32_t v, int) {
            ports[p] = v;
            auto effects = port92.write_register(v);
            cpu.a20 = effects.a20;
            resetRequested |= effects.reset;
        }));
    ioPorts.map("debug-console", 0xe9, 1,
        counted([this](uint16_t, int) -> uint32_t { return debugConsole.read_data_register(); }),
        bytes([this](uint16_t p, uint32_t v, int) {
            ports[p] = v;
            debugConsole.write_data_register(v);
        }));
}

} // namespace pc
