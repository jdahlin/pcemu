#include "usb_xhci.hpp"
#include <algorithm>
#include <stdexcept>
#include <sstream>
namespace pc {
namespace {
void access(uint64_t o, unsigned w, unsigned size) {
    if ((w != 1 && w != 2 && w != 4 && w != 8) || o % w || o >= size || w > size - o)
        throw std::runtime_error("Invalid xHCI register access");
}
const std::vector<uint8_t> keyboardDescriptor = {
    0x05, 1, 0x09, 6, 0xa1, 1,    0x05, 7, 0x19, 0xe0, 0x29, 0xe7, 0x15, 0, 0x25, 1,
    0x75, 1, 0x95, 8, 0x81, 2,    0x95, 1, 0x75, 8,    0x81, 1,    0x95, 5, 0x75, 1,
    0x05, 8, 0x19, 1, 0x29, 5,    0x91, 2, 0x95, 1,    0x75, 3,    0x91, 1, 0x95, 6,
    0x75, 8, 0x15, 0, 0x25, 0x65, 0x05, 7, 0x19, 0,    0x29, 0x65, 0x81, 0, 0xc0};
const std::vector<uint8_t> mouseDescriptor = {
    0x05, 1,    0x09, 2,    0xa1, 1,    0x09, 1,    0xa1, 0,    0x05, 9,    0x19,
    1,    0x29, 3,    0x15, 0,    0x25, 1,    0x95, 3,    0x75, 1,    0x81, 2,
    0x95, 1,    0x75, 5,    0x81, 1,    0x05, 1,    0x09, 0x30, 0x09, 0x31, 0x09,
    0x38, 0x15, 0x81, 0x25, 0x7f, 0x75, 8,    0x95, 3,    0x81, 6,    0xc0, 0xc0};
const std::vector<uint8_t> tabletDescriptor = {
    0x05,1, 0x09,2, 0xa1,1, 0x09,1, 0xa1,0,
    0x05,9, 0x19,1, 0x29,3, 0x15,0, 0x25,1, 0x95,3, 0x75,1, 0x81,2,
    0x95,1, 0x75,5, 0x81,1,
    0x05,1, 0x09,0x30, 0x09,0x31, 0x15,0, 0x26,0xff,0x7f,
    0x75,16, 0x95,2, 0x81,2,
    0x09,0x38, 0x15,0x81, 0x25,0x7f, 0x75,8, 0x95,1, 0x81,6, 0xc0,0xc0};
} // namespace
UsbXhci::UsbXhci(std::span<uint8_t> r, uint64_t b, std::function<void(bool)> i)
    : ram(r), ramBase(b), interrupt(std::move(i)) {
    reset();
}
void UsbXhci::range(uint64_t a, uint64_t n) const {
    if (a < ramBase || n > ram.size() || a - ramBase > ram.size() - n)
    {
        std::ostringstream detail;
        detail << "xHCI DMA outside guest RAM: address=0x" << std::hex << a
               << " length=0x" << n;
        throw std::runtime_error(detail.str());
    }
}
uint64_t UsbXhci::get(uint64_t a, unsigned w) const {
    range(a, w);
    uint64_t v = 0;
    for (unsigned i = 0; i < w; ++i)
        v |= uint64_t(ram[a - ramBase + i]) << (8 * i);
    return v;
}
void UsbXhci::put(uint64_t a, uint64_t v, unsigned w) {
    range(a, w);
    for (unsigned i = 0; i < w; ++i)
        ram[a - ramBase + i] = v >> (8 * i);
}
void UsbXhci::reset() {
    usbcmd = 0;
    status = 1;
    iman = 0;
    erstSize = 0;
    config = 0;
    imod = 0;
    failure.clear();
    crcr = dcbaa = erst = erdp = eventBase = eventIndex = eventCount = outstanding = 0;
    eventCycle = true;
    commandRing = {};
    slots = {};
    // Connected full-speed USB2 devices, port power on, link state polling.
    ports.fill(1 | (1 << 9) | (1 << 10) | (7 << 5));
    pending = {};
    keys = {};
    buttons = 0;
    tabletX = tabletY = 16384;
    irq();
}
void UsbXhci::irq() {
    interrupt((pciCommand & 0x400) == 0 && (usbcmd & 4) && (iman & 3) == 3);
}
uint64_t UsbXhci::configRead(uint64_t o, unsigned w) const {
    access(o, w, 4096);
    uint64_t v = 0;
    for (unsigned i = 0; i < w; ++i) {
        unsigned at = o + i;
        uint32_t d = 0;
        switch (at & ~3u) {
        case 0:
            d = 0x000d1b36;
            break; // QEMU generic xHCI compatible PCI identity
        case 4:
            d = pciCommand;
            break;
        case 8:
            d = 0x0c033001;
            break;
        case 0x10:
            d = probe ? 0xffffc000 : bar;
            break;
        case 0x2c:
            d = 0x000d1b36;
            break;
        case 0x3c:
            d = 0x1ff;
            break;
        }
        v |= uint64_t(uint8_t(d >> (8 * (at & 3)))) << (8 * i);
    }
    return v;
}
void UsbXhci::configWrite(uint64_t o, uint64_t v, unsigned w) {
    access(o, w, 4096);
    if (o == 0x10 && w == 4) {
        probe = uint32_t(v) == 0xffffffff;
        if (!probe)
            bar = v & 0xffffc000;
    }
    if (o == 4 && (w == 2 || w == 4)) {
        pciCommand = v & 0x406;
        irq();
    }
}
bool UsbXhci::contains(uint64_t a, unsigned w) const {
    return (pciCommand & 2) && bar >= 0x10000000 && bar < 0x3f000000 && a >= bar &&
           a - bar < 0x4000 && w <= 0x4000 - (a - bar);
}
uint32_t UsbXhci::reg(unsigned o) const {
    switch (o) {
    case 0:
        return 0x01000040; // version 1.0, operational offset 0x40
    case 4:
        return (3u << 24) | (1 << 8) | 8; // ports, interrupters, slots
    case 0x10:
        return 1 | (0xc00u << 16); // 64-bit addresses, 32-byte contexts
    case 0x14:
        return 0x2000; // doorbells
    case 0x18:
        return 0x1000; // runtime
    case 0x40:
        return usbcmd;
    case 0x44:
        return status;
    case 0x48:
        return 1; // 4 KiB page
    case 0x58:
        return crcr;
    case 0x5c:
        return crcr >> 32;
    case 0x70:
        return dcbaa;
    case 0x74:
        return dcbaa >> 32;
    case 0x78:
        return config;
    case 0x1020:
        return iman;
    case 0x1024:
        return imod;
    case 0x1028:
        return erstSize;
    case 0x1030:
        return erst;
    case 0x1034:
        return erst >> 32;
    case 0x1038:
        return erdp;
    case 0x103c:
        return erdp >> 32;
    case 0x3000:
        return 2 | (2u << 24); // USB 2.0 Supported Protocol capability
    case 0x3004:
        return 0x20425355; // "USB "
    case 0x3008:
        return 1 | (3 << 8); // first port, port count
    default:
        if (o == 0x440 || o == 0x450 || o == 0x460)
            return ports[(o - 0x440) / 16];
        return 0;
    }
}
uint64_t UsbXhci::read(uint64_t a, unsigned w) {
    auto o = a - bar;
    access(o, w, 0x4000);
    uint64_t v = 0;
    for (unsigned i = 0; i < w; ++i)
        v |= uint64_t(uint8_t(reg((o + i) & ~3u) >> (8 * ((o + i) & 3)))) << (8 * i);
    return v;
}
void UsbXhci::regWrite(unsigned o, uint32_t v) {
    auto part = [&](uint64_t &r, bool high) {
        r = high ? (r & 0xffffffffull) | (uint64_t(v) << 32) : (r & 0xffffffff00000000ull) | v;
    };
    switch (o) {
    case 0x40:
        if (v & 2) {
            reset();
            break;
        }
        usbcmd = v & 0x40d;
        status = (status & ~1u) | ((v & 1) ? 0 : 1);
        irq();
        break;
    case 0x44:
        status &= ~(v & 0x41c);
        irq();
        break;
    case 0x58:
        part(crcr, false);
        commandRing = {crcr & ~63ull, bool(crcr & 1)};
        break;
    case 0x5c:
        part(crcr, true);
        commandRing = {crcr & ~63ull, bool(crcr & 1)};
        break;
    case 0x70:
        part(dcbaa, false);
        break;
    case 0x74:
        part(dcbaa, true);
        break;
    case 0x78:
        config = v & 0xff;
        break;
    case 0x1020:
        iman = (iman & 1 & ~(v & 1)) | (v & 2);
        irq();
        break;
    case 0x1024:
        imod = v;
        break;
    case 0x1028:
        erstSize = v & 0xffff;
        break;
    case 0x1030:
        part(erst, false);
        eventBase = 0;
        break;
    case 0x1034:
        part(erst, true);
        eventBase = 0;
        break;
    case 0x1038:
        part(erdp, false);
        erdp &= ~8ull;
        outstanding = 0;
        break;
    case 0x103c:
        part(erdp, true);
        break;
    default:
        if (o == 0x440 || o == 0x450 || o == 0x460) {
            auto &p = ports[(o - 0x440) / 16];
            p &= ~(v & 0xfe0000); // RW1C change bits
            if (v & 0x10) {
                p = (p & ~(0x10u | 0x1e0u)) | 2 | (1 << 21); // reset complete, enabled, U0
                if (usbcmd & 1)
                    event(uint64_t((o - 0x440) / 16 + 1) << 24, 1u << 24, 34u << 10);
            }
        }
        break;
    }
}
void UsbXhci::write(uint64_t a, uint64_t v, unsigned w) {
    auto o = a - bar;
    access(o, w, 0x4000);
    if (w != 4 && w != 8)
        throw std::runtime_error("xHCI writes require dwords");
    try {
        if (o >= 0x2000 && o < 0x2024) {
            if ((pciCommand & 4) && (usbcmd & 1))
                doorbell((o - 0x2000) / 4, v & 0xff);
        } else {
            regWrite(o, v);
            if (w == 8)
                regWrite(o + 4, v >> 32);
        }
    } catch (const std::runtime_error &e) {
        failure = e.what();
        status |= 4;
        usbcmd &= ~1u;
        status |= 1;
        irq();
    }
}
void UsbXhci::event(uint64_t p, uint32_t s, uint32_t c) {
    if (!(pciCommand & 4))
        return;
    if (!eventBase) {
        if (erstSize != 1)
            throw std::runtime_error("xHCI needs one event segment");
        eventBase = get(erst, 8);
        eventCount = get(erst + 8, 4);
        if (!eventCount || eventCount > 4096 || eventBase % 64)
            throw std::runtime_error("Invalid xHCI event ring");
        range(eventBase, eventCount * 16);
        eventIndex = 0;
        eventCycle = true;
    }
    if (outstanding >= eventCount - 1)
        throw std::runtime_error("xHCI event ring full");
    auto a = eventBase + eventIndex * 16;
    put(a, p, 8);
    put(a + 8, s, 4);
    put(a + 12, c | unsigned(eventCycle), 4);
    if (++eventIndex == eventCount) {
        eventIndex = 0;
        eventCycle = !eventCycle;
    }
    ++outstanding;
    iman |= 1;
    status |= 8;
    irq();
}
bool UsbXhci::next(Ring &r, std::array<uint32_t, 4> &t, uint64_t &a) {
    for (unsigned n = 0; n < 256; ++n) {
        if (!r.pointer || r.pointer % 16)
            throw std::runtime_error("Invalid xHCI dequeue pointer");
        range(r.pointer, 16);
        for (unsigned i = 0; i < 4; ++i)
            t[i] = get(r.pointer + i * 4, 4);
        if (bool(t[3] & 1) != r.cycle)
            return false;
        a = r.pointer;
        if (((t[3] >> 10) & 63) != 6) {
            r.pointer += 16;
            return true;
        }
        r.pointer = (uint64_t(t[1]) << 32 | t[0]) & ~15ull;
        if (t[3] & 2)
            r.cycle = !r.cycle;
    }
    throw std::runtime_error("Cyclic xHCI link ring");
}
void UsbXhci::context(unsigned slot, unsigned ep, unsigned state) {
    auto out = get(dcbaa + slot * 8, 8);
    range(out, 32 * 32);
    auto at = out + ep * 32;
    put(at, (get(at, 4) & ~7ull) | state, 4);
    if (ep)
        put(at + 8, slots[slot].endpoints[ep].pointer | slots[slot].endpoints[ep].cycle, 8);
}
void UsbXhci::command() {
    std::array<uint32_t, 4> t;
    uint64_t a;
    for (unsigned n = 0; n < 256 && next(commandRing, t, a); ++n) {
        ++commands;
        unsigned type = (t[3] >> 10) & 63, id = t[3] >> 24, cc = 1;
        auto input = (uint64_t(t[1]) << 32 | t[0]) & ~15ull;
        if (type == 9) {
            id = 1;
            while (id < slots.size() && slots[id].enabled)
                ++id;
            if (id == slots.size() || id > config) {
                cc = 9;
                id = 0;
            } else {
                slots[id] = {};
                slots[id].enabled = true;
            }
        } else if (type == 23) { // No-op
        } else if (!id || id >= slots.size() || !slots[id].enabled)
            cc = 11;
        else {
            auto &s = slots[id];
            if (type == 10)
                s = {};
            else if (type == 13) { // Evaluate Context updates selected fields only.
                range(input, 33 * 32);
                auto out = get(dcbaa + id * 8, 8);
                range(out, 32 * 32);
                auto add = get(input + 4, 4);
                if (get(input, 4) || (add & ~3ull))
                    cc = 5;
                else {
                    if (add & 1) {
                        put(out + 4, (get(out + 4, 4) & ~0xffffull) |
                            (get(input + 36, 4) & 0xffff), 4);
                        put(out + 8, (get(out + 8, 4) & ~0xffc00000ull) |
                            (get(input + 40, 4) & 0xffc00000), 4);
                    }
                    if (add & 2)
                        put(out + 36, (get(out + 36, 4) & ~0xffff0000ull) |
                            (get(input + 68, 4) & 0xffff0000), 4);
                }
            } else if (type == 11 || type == 12) {
                range(input, 33 * 32);
                auto out = get(dcbaa + id * 8, 8);
                range(out, 32 * 32);
                uint32_t drop = get(input, 4), add = get(input + 4, 4);
                if (type == 11)
                    add = 3;
                if (type == 11) {
                    s.port = (get(input + 32 + 4, 4) >> 16) & 255;
                    if (s.port < 1 || s.port > 3)
                        cc = 17;
                }
                if (cc == 1) {
                    for (unsigned ep = type == 12 ? 2 : 0; ep < 32; ++ep) {
                        if (drop & (1u << ep)) {
                            s.active[ep] = false;
                            context(id, ep, 0);
                        }
                        if (add & (1u << ep)) {
                            for (unsigned j = 0; j < 32; j += 4)
                                put(out + ep * 32 + j, get(input + (ep + 1) * 32 + j, 4), 4);
                            if (ep) {
                                auto dq = get(input + (ep + 1) * 32 + 8, 8);
                                s.endpoints[ep] = {dq & ~15ull, bool(dq & 1)};
                                s.active[ep] = false;
                                context(id, ep, 1);
                            }
                        }
                    }
                    if (type == 11)
                        put(out + 12, (t[3] & (1 << 9)) ? (1u << 27) : (id & 255) | (2u << 27), 4);
                    if (type == 12) {
                        // Configure Endpoint consumes Context Entries, not a
                        // replacement slot address/root port or EP0 context.
                        put(out, (get(out, 4) & 0x07ffffff) |
                            (get(input + 32, 4) & 0xf8000000), 4);
                        auto state = get(out + 12, 4);
                        put(out + 12, (state & 0x7ffffff) | (3u << 27), 4);
                    }
                }
            } else if (type == 14 || type == 15 || type == 16) {
                unsigned ep = (t[3] >> 16) & 31;
                if (!ep)
                    cc = 12;
                else {
                    s.active[ep] = false;
                    if (type == 16)
                        s.endpoints[ep] = {input, bool(t[0] & 1)};
                    context(id, ep, type == 15 ? 3 : 1);
                }
            } else if (type == 17) {
                for (unsigned ep = 2; ep < 32; ++ep) {
                    s.active[ep] = false;
                    context(id, ep, 0);
                }
            } else
                cc = 5;
        }
        event(a, cc << 24, (33u << 10) | (id << 24));
    }
}
void UsbXhci::setup(Slot &s, uint64_t p) {
    unsigned request = (p >> 8) & 255, value = (p >> 16) & 65535, length = p >> 48;
    const auto &report = s.port == 1 ? keyboardDescriptor : s.port == 2 ? mouseDescriptor : tabletDescriptor;
    s.control.clear();
    s.controlIn = p & 0x80;
    if (request == 6) {
        switch (value >> 8) {
        case 1:
            s.control = {18, 1, 0x10, 1, 0, 0, 0, 8, 0x36, 0x1b, uint8_t(s.port),
                         0,  0, 1,    0, 0, 0, 1};
            break;
        case 2: {
            s.control = {9,
                         2,
                         34,
                         0,
                         1,
                         1,
                         0,
                         0xa0,
                         50,
                         9,
                         4,
                         0,
                         0,
                         1,
                         3,
                         uint8_t(s.port == 3 ? 0 : 1),
                         uint8_t(s.port == 1 ? 1 : s.port == 2 ? 2 : 0),
                         0,
                         9,
                         0x21,
                         0x11,
                         1,
                         0,
                         1,
                         0x22,
                         uint8_t(report.size()),
                         0,
                         7,
                         5,
                         0x81,
                         3,
                         8,
                         0,
                         10};
            break;
        }
        case 0x21:
            s.control = {9, 0x21, 0x11, 1, 0, 1, 0x22, uint8_t(report.size()), 0};
            break;
        case 0x22:
            s.control = report;
            break;
        default:
            throw std::runtime_error("Unsupported USB descriptor");
        }
    } else if (request == 9 && !(p & 0x60))
        s.configuration = value;
    else if (request == 9 && (p & 0x60) == 0x20) {
    } // HID SET_REPORT LEDs
    else if (request == 8)
        s.control = {uint8_t(s.configuration)};
    else if (request == 0)
        s.control = {0, 0};
    else if (request == 10 && (p & 0x60) == 0x20)
        s.idle = value >> 8;
    else if (request == 11 && (p & 0x60) == 0x20)
        s.protocol = value;
    else if (request == 3 && (p & 0x60) == 0x20)
        s.control = {uint8_t(s.protocol)};
    else if (request == 2 && (p & 0x60) == 0x20)
        s.control = {uint8_t(s.idle)};
    else if (request == 1 && (p & 0x60) == 0x20)
        s.control = std::vector<uint8_t>(s.port == 1 ? 8 : s.port == 2 ? 4 : 6);
    else if (request == 1 || request == 3 || request == 5 || request == 11 || request == 0x0a) {
    } else
        throw std::runtime_error("Unsupported USB request");
    if (s.control.size() > length)
        s.control.resize(length);
}
void UsbXhci::transfer(unsigned id, unsigned ep) {
    auto &s = slots[id];
    auto &r = s.endpoints[ep];
    std::array<uint32_t, 4> t;
    uint64_t a;
    for (unsigned n = 0; n < 256; ++n) {
        auto saved = r;
        if (!next(r, t, a)) {
            s.active[ep] = false;
            return;
        }
        unsigned type = (t[3] >> 10) & 63, length = t[2] & 0x1ffff, actual = 0, cc = 1;
        uint64_t ptr = uint64_t(t[1]) << 32 | t[0];
        if (type == 1 && ep == 3 && pending[s.port - 1].empty()) {
            r = saved;
            return;
        } // USB NAK

        if (type == 2 && ep == 1) {
            try {
                setup(s, ptr);
            } catch (const std::runtime_error &) {
                cc = 6;
            }
        } else if ((type == 3 || type == 1) && ep == 1) {
            if (type == 3 ? bool(t[3] & (1 << 16)) : s.controlIn) {
                actual = std::min<size_t>(length, s.control.size());
                if (actual) {
                    range(ptr, actual);
                    std::copy_n(s.control.begin(), actual, ram.begin() + (ptr - ramBase));
                }
                s.control.erase(s.control.begin(), s.control.begin() + actual);
                if (actual < length)
                    cc = 13;
            } else if (t[3] & 64) {
                // OUT data may live inline in the TRB, including a zero LED
                // byte. IDT is payload, never a guest physical DMA pointer.
                if (length > 8) cc = 5;
                else actual = length;
            } else {
                if (length) range(ptr, length);
                actual = length;
            }
        } else if (type == 1 && ep == 3) {
            auto &q = pending[s.port - 1];
            actual = std::min<size_t>(length, s.port == 2 && !s.protocol ? 3 : q.front().size());
            if (actual) {
                range(ptr, actual);
                std::copy_n(q.front().begin(), actual, ram.begin() + (ptr - ramBase));
            }
            q.pop_front();
            ++reports;
            if (actual < length)
                cc = 13;
        } else if (type != 4 && type != 8 && type != 7)
            cc = 5;
        ++transfers;
        // Event Data completes the preceding TD, including its short-packet
        // status. Windows uses this completion to size descriptor responses.
        if (type == 2 || type == 4) {
            s.actual[ep] = 0;
            s.shortPacket[ep] = false;
        }
        if (cc == 13)
            s.shortPacket[ep] = true;
        if (type == 7 && s.shortPacket[ep])
            cc = 13;
        s.actual[ep] += actual;
        if ((t[3] & 32) || (cc == 13 && (t[3] & 4)) || (cc != 1 && cc != 13)) {
            event(type == 7 ? ptr : a, (cc << 24) | (type == 7 ? s.actual[ep] : length - actual),
                  (32u << 10) | (ep << 16) | (id << 24) | (type == 7 ? 4 : 0));
            s.actual[ep] = 0;
        }
        if (!(t[3] & 16))
            s.shortPacket[ep] = false;
        context(id, ep, cc == 6 ? 2 : 1);
        if (cc != 1 && cc != 13) {
            s.active[ep] = false;
            return;
        }
    }
}
void UsbXhci::doorbell(unsigned id, unsigned ep) {
    if (!id) {
        command();
        return;
    }
    if (id >= slots.size() || !slots[id].enabled || !slots[id].port || (ep != 1 && ep != 3))
        return;
    slots[id].active[ep] = true;
    transfer(id, ep);
}
void UsbXhci::queue(unsigned port, std::vector<uint8_t> report) {
    auto &q = pending[port - 1];
    if (q.size() == 256)
        q.pop_front();
    q.push_back(std::move(report));
    if ((pciCommand & 4) && (usbcmd & 1)) {
        try {
            for (unsigned id = 1; id < slots.size(); ++id)
                if (slots[id].port == port && slots[id].active[3])
                    transfer(id, 3);
        } catch (const std::runtime_error &e) {
            failure = e.what();
            status |= 5;
            usbcmd &= ~1u;
            irq();
        }
    }
}
void UsbXhci::key(uint16_t k, bool released) {
    ++keyInputs;
    if (k >= 256 || keys[k] == !released)
        return;
    keys[k] = !released;
    std::vector<uint8_t> report(8);
    unsigned count = 0;
    for (unsigned i = 0xe0; i <= 0xe7; ++i)
        if (keys[i])
            report[0] |= 1 << (i - 0xe0);
    for (unsigned i = 4; i < 0xe0; ++i)
        if (keys[i]) {
            if (count < 6)
                report[2 + count] = i;
            ++count;
        }
    if (count > 6)
        std::fill(report.begin() + 2, report.end(), 1);
    queue(1, std::move(report));
}
void UsbXhci::mouse(int dx, int dy, int wheel, uint8_t b) {
    ++pointerInputs;
    buttons = b & 7;
    do {
        int x = std::clamp(dx, -127, 127), y = std::clamp(dy, -127, 127),
            z = std::clamp(wheel, -127, 127);
        queue(2, {buttons, uint8_t(x), uint8_t(y), uint8_t(z)});
        dx -= x;
        dy -= y;
        wheel -= z;
    } while (dx || dy || wheel);
}
void UsbXhci::absoluteMouse(int x, int y, int wheel, uint8_t b) {
    ++pointerInputs;
    tabletX = unsigned(std::clamp(x, 0, 65535)) * 32767u / 65535u;
    tabletY = unsigned(std::clamp(y, 0, 65535)) * 32767u / 65535u;
    do {
        int z = std::clamp(wheel, -127, 127);
        queue(3, {uint8_t(b & 7), uint8_t(tabletX), uint8_t(tabletX >> 8),
                  uint8_t(tabletY), uint8_t(tabletY >> 8), uint8_t(z)});
        wheel -= z;
    } while (wheel);
}
void UsbXhci::releaseAll() {
    keys = {};
    buttons = 0;
    queue(1, std::vector<uint8_t>(8));
    queue(2, std::vector<uint8_t>(4));
    queue(3, {0, uint8_t(tabletX), uint8_t(tabletX >> 8), uint8_t(tabletY), uint8_t(tabletY >> 8), 0});
}
} // namespace pc

namespace pc {
std::string UsbXhci::inputState() const {
    std::string result = " keys=" + std::to_string(keyInputs) +
        " pointers=" + std::to_string(pointerInputs);
    for (unsigned id = 1; id < slots.size(); ++id) {
        const auto &slot = slots[id];
        if (slot.enabled) result += " slot" + std::to_string(id) + "=port" +
            std::to_string(slot.port) + ",config" + std::to_string(slot.configuration) +
            ",waiting" + std::to_string(slot.active[3]);
    }
    return result;
}
}
