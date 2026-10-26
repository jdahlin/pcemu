#include "usb_guest_input.hpp"
#include <cassert>
#include <iostream>
using namespace pc;
struct Guest {
    static constexpr uint64_t base = 0x40000000, bar = 0x10000000;
    std::vector<uint8_t> ram = std::vector<uint8_t>(1 << 20);
    bool irq = false;
    UsbXhci usb{ram, base, [this](bool v) { irq = v; }};
    uint64_t command = base + 0x1000, events = base + 0x3000, input = base + 0x4000;
    uint64_t output = base + 0x5000, control = base + 0x6000, data = base + 0x7000,
             intr = base + 0x8000;
    void put(uint64_t a, uint64_t v, unsigned n = 4) {
        for (unsigned i = 0; i < n; ++i)
            ram.at(a - base + i) = v >> (8 * i);
    }
    uint64_t get(uint64_t a, unsigned n = 4) {
        uint64_t v = 0;
        for (unsigned i = 0; i < n; ++i)
            v |= uint64_t(ram.at(a - base + i)) << (8 * i);
        return v;
    }
    void trb(uint64_t a, uint64_t p, uint32_t s, uint32_t c) {
        put(a, p, 8);
        put(a + 8, s);
        put(a + 12, c | 1);
    }
    void w(unsigned o, uint64_t v, unsigned n = 4) {
        usb.write(bar + o, v, n);
    }
    uint64_t r(unsigned o, unsigned n = 4) {
        return usb.read(bar + o, n);
    }
    Guest() {
        usb.configWrite(0x10, bar, 4);
        usb.configWrite(4, 6, 2);
        put(base + 0x2000, events, 8);
        put(base + 0x2008, 256);
        w(0x1028, 1);
        w(0x1030, base + 0x2000, 8);
        w(0x1038, events, 8);
        w(0x70, base + 0x9000, 8);
        put(base + 0x9008, output, 8);
        w(0x78, 8);
        w(0x58, command | 1, 8);
        w(0x1020, 2);
        w(0x40, 5);
    }
    void cmd(uint32_t type, uint64_t p = 0, unsigned slot = 1) {
        trb(command, p, 0, (type << 10) | (slot << 24));
        command += 16;
        w(0x2000, 0);
    }
    void device(unsigned port) {
        cmd(9);
        assert(get(events + 8) >> 24 == 1);
        assert(get(events + 12) >> 24 == 1);
        put(input + 4, 3);
        put(input + 36, port << 16);
        put(input + 64 + 8, control | 1, 8);
        cmd(11, input);
        assert(get(events + 24) >> 24 == 1);
    }
    void request(uint64_t packet, unsigned length) {
        trb(control, packet, 8, (2 << 10) | 64 | (3 << 16));
        trb(control + 16, data, length, (3 << 10) | (1 << 16) | 4);
        trb(control + 32, 0, 0, (4 << 10) | 32);
        w(0x2004, 1);
        control += 48;
    }
    void interruptEndpoint() {
        auto portAndLatency = get(output + 4), address = get(output + 12) & 255;
        put(input + 4, 1 | 8);
        put(input + 36, 0); // Configure Endpoint does not supply the root port.
        put(input + 128 + 8, intr | 1, 8);
        cmd(12, input);
        assert(get(output + 4) == portAndLatency);
        assert((get(output + 12) & 255) == address);
        assert((get(output + 12) >> 27) == 3);
    }
};
struct TestFrontend : Frontend {
    bool captured = false;
    std::vector<FrontendEvent> poll() override { return {}; }
    void present(std::span<const uint32_t>, unsigned, unsigned, DisplayShape) override {}
    bool capturePointer(bool enabled) override { captured = enabled; return true; }
};
int main() {
    {
        Guest g; g.device(3);
        g.request((uint64_t(64) << 48) | (uint64_t(0x2200) << 16) | 0x681, 64);
        assert(g.get(g.data, 2) == 0x105);
        g.interruptEndpoint();
        UsbGuestInput input(g.usb);
        input.absolutePointer(65535, 0, -1, 5);
        g.trb(g.intr, g.data, 8, (1 << 10) | 32); g.w(0x2004, 3);
        assert(g.get(g.data, 6) == 0xff00007fff05ull);
        g.trb(g.intr + 16, g.data, 8, (1 << 10) | 32); g.w(0x2004, 3);
        input.release();
        assert(g.get(g.data, 6) == 0x0000007fff00ull); // Release buttons without moving.
        assert(g.usb.error().empty());
    }

    {
        Guest g;
        assert(g.usb.configRead(0, 4) == 0x000d1b36);
        g.usb.configWrite(0x10, 0xffffffff, 4);
        assert(g.usb.configRead(0x10, 4) == 0xffffc000);
        g.usb.configWrite(0x10, Guest::bar, 4);
        assert(g.r(4) == ((3u << 24) | (1 << 8) | 8));
        assert(g.r(0x3008) == 0x301);
        g.device(1);
        assert(g.irq);
        g.w(0x1020, 1);
        assert(!g.irq); // IE can be cleared
        g.w(0x1020, 2);
        g.w(0x440, 0x210);
        assert(g.irq);
        assert(g.r(0x440) & 2);
        // GET_DESCRIPTOR device, configuration, HID report, short packets.
        g.request((uint64_t(18) << 48) | (uint64_t(0x100) << 16) | 0x680, 18);
        assert(g.get(g.data, 2) == 0x112);
        assert(g.get(g.data + 7, 1) == 8);
        g.request((uint64_t(64) << 48) | (uint64_t(0x200) << 16) | 0x680, 64);
        assert(g.get(g.data + 2, 2) == 34);
        assert(g.get(g.data + 30, 1) == 3);
        g.request((uint64_t(255) << 48) | (uint64_t(0x2200) << 16) | 0x681, 255);
        assert(g.get(g.data, 2) == 0x105);
        g.interruptEndpoint();
        g.trb(g.intr, g.data, 8, (1 << 10) | 32);
        g.w(0x2004, 3);
        assert(g.usb.reports == 0); // interrupt transfer stays pending on NAK
        g.usb.key(0xe1, false);
        assert(g.usb.reports == 1);
        assert(g.get(g.data, 1) == 2);
        g.trb(g.intr + 16, g.data, 8, (1 << 10) | 32);
        g.w(0x2004, 3);
        g.usb.key(4, false);
        assert(g.get(g.data + 2, 1) == 4);
        g.trb(g.intr + 32, g.data, 8, (1 << 10) | 32);
        g.w(0x2004, 3);
        g.usb.releaseAll();
        assert(g.get(g.data, 8) == 0);
        g.w(0x40, 2);
        assert(g.r(0x44) == 1);
        assert(!g.irq);
    }
    {
        Guest g;
        g.device(2);
        g.interruptEndpoint();
        g.trb(g.intr, g.data, 4, (1 << 10) | 32);
        g.w(0x2004, 3);
        g.usb.mouse(-15, 20, -1, 5);
        assert(g.get(g.data, 4) == 0xff14f105);
    }
    {
        Guest g;
        g.device(1);
        g.interruptEndpoint();
        // Windows-style chained TD followed by Event Data: it must complete
        // immediately, even when the HID report queue becomes empty.
        g.trb(g.intr, g.data, 8, (1 << 10) | 16);
        g.trb(g.intr + 16, 0x123456789abc, 0, (7 << 10) | 32);
        g.w(0x2004, 3);
        UsbGuestInput input(g.usb);
        input.key(0xe3, false);
        input.consumeCommand();
        input.key(0xe3, true);
        assert(g.usb.reports == 0); // host Cmd+G never opens guest Start
        input.key(4, false);
        assert(g.usb.reports == 1);
        auto e = g.events + 3 * 16; // enable, address, configure, transfer
        assert(g.get(e, 8) == 0x123456789abc);
        assert(g.get(e + 8) == 0x01000008);
        assert(g.get(e + 12) & 4); // event-data flag
    }
    {
        Guest g;
        g.device(1);
        // Windows separates the data and status stages with Event Data TRBs.
        // A short configuration descriptor must retain CC=Short Packet at ED.
        g.trb(g.control, (uint64_t(255) << 48) | (uint64_t(0x200) << 16) | 0x680,
              8, (2 << 10) | 64 | (3 << 16));
        g.trb(g.control + 16, g.data, 255, (3 << 10) | (1 << 16) | 16 | 2);
        g.trb(g.control + 32, 0x1111, 0, (7 << 10) | 32);
        g.trb(g.control + 48, 0, 0, (4 << 10) | 16);
        g.trb(g.control + 64, 0x2222, 0, (7 << 10) | 32);
        g.w(0x2004, 1);
        auto dataEvent = g.events + 2 * 16;
        assert(g.get(dataEvent, 8) == 0x1111);
        assert(g.get(dataEvent + 8) == (13u << 24 | 34));
        assert(g.get(dataEvent + 12) & 4);
        auto statusEvent = dataEvent + 16;
        assert(g.get(statusEvent, 8) == 0x2222);
        assert(g.get(statusEvent + 8) == (1u << 24));
    }
    {
        Guest g;
        g.device(1);
        // Windows only supplies Max Exit Latency / Interrupter Target when
        // evaluating a slot, not the already configured root-hub port.
        auto slotState = g.get(g.output + 12);
        auto dequeue = g.get(g.output + 40, 8);
        g.put(g.input + 4, 1);
        g.put(g.input + 36, 123);
        g.cmd(13, g.input);
        assert(g.get(g.events + 2 * 16 + 8) >> 24 == 1);
        assert(g.get(g.output + 4) == ((1u << 16) | 123));
        assert(g.get(g.output + 12) == slotState);
        // EP0 evaluation may update MPS, but never its dequeue pointer/state.
        g.put(g.input + 4, 2);
        g.put(g.input + 68, 64u << 16);
        g.put(g.input + 72, 0, 8);
        g.cmd(13, g.input);
        assert(g.get(g.output + 36) >> 16 == 64);
        assert(g.get(g.output + 40, 8) == dequeue);
        assert((g.get(g.output + 32) & 7) == 1);
        g.request((uint64_t(18) << 48) | (uint64_t(0x100) << 16) | 0x680, 18);
        assert(g.get(g.data, 2) == 0x112); // port and ring still usable
    }
    {
        Guest g;
        g.device(1);
        g.interruptEndpoint();
        g.trb(g.intr, g.data, 8, (1 << 10) | 32);
        g.trb(g.intr + 16, g.data + 8, 8, (1 << 10) | 32);
        g.w(0x2004, 3);
        UsbGuestInput guest(g.usb);
        TestFrontend ui;
        FrontendInput input(&ui, guest);
        input.event({FrontendEvent::Kind::Key, 0x28});
        input.event({FrontendEvent::Kind::Key, 0x28, true});
        assert(g.usb.reports == 2);
        assert(g.get(g.data + 2, 1) == 0x28 && g.get(g.data + 8, 8) == 0);
    }
    {
        Guest g;
        g.device(2);
        g.interruptEndpoint();
        g.trb(g.intr, g.data, 4, (1 << 10) | 32);
        g.w(0x2004, 3);
        UsbGuestInput guest(g.usb);
        TestFrontend ui;
        FrontendInput input(&ui, guest);
        input.event({FrontendEvent::Kind::Button, 0, false, 1});
        assert(ui.captured && g.usb.reports == 0); // capture click consumed
        input.event({FrontendEvent::Kind::Motion, 0, false, 0, 12, -8});
        assert(g.usb.reports == 1 && g.get(g.data, 4) == 0x00f80c00);
        g.trb(g.intr + 16, g.data, 4, (1 << 10) | 32);
        g.w(0x2004, 3);
        input.event({FrontendEvent::Kind::Button, 0, false, 1});
        assert(g.get(g.data, 1) == 1);
        g.trb(g.intr + 32, g.data, 4, (1 << 10) | 32);
        g.w(0x2004, 3);
        input.event({FrontendEvent::Kind::FocusLost});
        assert(!ui.captured && g.get(g.data, 4) == 0);
    }
    {
        Guest g;
        g.device(1);
        // Windows initializes keyboard LEDs with inline OUT report data.
        // IDT payload zero is not a DMA address at physical address zero.
        g.trb(g.control, (uint64_t(1) << 48) | (uint64_t(0x200) << 16) | 0x921,
              8, (2 << 10) | 64 | (2 << 16));
        g.trb(g.control + 16, 0, 1, (3 << 10) | 64 | 16);
        g.trb(g.control + 32, 0, 0, (4 << 10) | (1 << 16) | 32);
        g.w(0x2004, 1);
        assert(g.usb.error().empty());
        assert(!(g.r(0x44) & 4));
    }
    {
        Guest g;
        g.w(0x58, Guest::base + g.ram.size() + 1, 8);
        g.w(0x2000, 0);
        assert((g.r(0x44) & 5) == 5); // bad DMA halts controller, never touches host memory
    }
    {
        Guest g;
        g.trb(g.command, g.command, 0, (6 << 10));
        g.w(0x2000, 0);
        assert((g.r(0x44) & 5) == 5); // cyclic link has bounded processing
    }
    std::cout << "USB xHCI enumeration, HID, IRQ, reset and malformed DMA passed\n";
}
