#include "ne2000.hpp"
#include <algorithm>
#include <cstring>
namespace pc {
NE2000::NE2000() {
    reset();
}
void NE2000::reset() {
    command = 0x21;
    isr = 0x80;
    imr = 0;
    rcr = tcr = dcr = 0;
    remote = remoteCount = transmitCount = local = 0;
    start = stop = boundary = current = transmitPage = 0;
    transmitStatus = receiveStatus = 0;
    tally.fill(0);
    multicast.fill(0);
    outgoing.clear();
    memory.fill(0);
    constexpr uint8_t mac[] = {0x02, 0x50, 0x43, 0x45, 0x4d, 0x01};
    std::copy_n(mac, 6, physical.begin());
    // NE2000's 16-byte station PROM is duplicated on the 16-bit data bus.
    for (unsigned i = 0; i < 16; ++i) {
        uint8_t b = i < 6 ? mac[i] : (i >= 14 ? 0x57 : 0);
        memory[i * 2] = memory[i * 2 + 1] = b;
    }
}
uint8_t NE2000::readMemory(uint16_t a) const {
    return a < 32 || (a >= 0x4000 && a < 0x8000) ? memory[a] : 0xff;
}
void NE2000::writeMemory(uint16_t a, uint8_t v) {
    if (a >= 0x4000 && a < 0x8000)
        memory[a] = v;
}
void NE2000::advanceDMA() {
    ++remote;
    if (stop > start && remote == uint16_t(stop) * 256)
        remote = uint16_t(start) * 256;
    if (remoteCount && --remoteCount == 0)
        isr |= 0x40;
}
uint8_t NE2000::dmaRead() {
    if (!remoteCount || (command & 0x38) != 0x08)
        return 0xff;
    uint8_t v = readMemory(remote);
    advanceDMA();
    return v;
}
void NE2000::dmaWrite(uint8_t v) {
    if (!remoteCount || (command & 0x38) != 0x10)
        return;
    writeMemory(remote, v);
    advanceDMA();
}
uint32_t NE2000::in(uint16_t p, int w) {
    unsigned r = p - base;
    if (r >= 0x10 && r <= 0x17) {
        uint32_t v = 0;
        for (int i = 0; i < w; ++i)
            v |= uint32_t(dmaRead()) << (8 * i);
        // WTS selects word-wide remote DMA even for a byte host read:
        // the unused upper lane is consumed, rather than read next time.
        if (w == 1 && (dcr & 1))
            dmaRead();
        return v;
    }
    if (r == 0x1f) {
        reset();
        return 0;
    }
    if (r == 0)
        return command;
    if (r >= 16)
        return 0xff;
    unsigned page = command >> 6;
    if (page == 1) {
        if (r <= 6)
            return physical[r - 1];
        if (r == 7)
            return current;
        return multicast[r - 8];
    }
    if (page == 2) {
        switch (r) {
        case 1:
            return start;
        case 2:
            return stop;
        case 3:
            return remote >> 8;
        case 4:
            return transmitPage;
        case 5:
            return local >> 8;
        case 6:
            return 0;
        case 7:
            return 0;
        case 12:
            return rcr;
        case 13:
            return tcr;
        case 14:
            return dcr;
        case 15:
            return imr;
        default:
            return 0;
        }
    }
    if (page != 0)
        return 0xff;
    switch (r) {
    case 1:
        return local & 255;
    case 2:
        return local >> 8;
    case 3:
        return boundary;
    case 4:
        return transmitStatus;
    case 5:
    case 6:
        return 0;
    case 7:
        return isr;
    case 8:
        return remote & 255;
    case 9:
        return remote >> 8;
    case 12:
        return receiveStatus;
    case 13:
    case 14:
    case 15: {
        uint8_t v = tally[r - 13];
        tally[r - 13] = 0;
        return v;
    }
    default:
        return 0;
    }
}
void NE2000::out(uint16_t p, uint32_t v, int w) {
    unsigned r = p - base;
    if (r >= 0x10 && r <= 0x17) {
        for (int i = 0; i < w; ++i)
            dmaWrite(v >> (i * 8));
        if (w == 1 && (dcr & 1) && remoteCount)
            advanceDMA();
        return;
    }
    if (r == 0x1f) {
        isr |= 0x80;
        return;
    }
    if (r == 0) {
        command = uint8_t(v) & ~4;
        if (command & 1)
            isr |= 0x80;
        else if (command & 2)
            isr &= ~0x80;
        if (((command & 0x38) == 0x08 || (command & 0x38) == 0x10) && !remoteCount)
            isr |= 0x40;
        if ((v & 4) && !(command & 1) && (command & 2))
            transmit();
        return;
    }
    if (r >= 16)
        return;
    unsigned page = command >> 6;
    if (page == 1) {
        if (r <= 6)
            physical[r - 1] = v;
        else if (r == 7)
            current = v;
        else
            multicast[r - 8] = v;
        return;
    }
    if (page != 0)
        return;
    switch (r) {
    case 1:
        start = v;
        break;
    case 2:
        stop = v;
        break;
    case 3:
        boundary = v;
        break;
    case 4:
        transmitPage = v;
        break;
    case 5:
        transmitCount = (transmitCount & 0xff00) | uint8_t(v);
        break;
    case 6:
        transmitCount = (transmitCount & 255) | uint16_t(v << 8);
        break;
    case 7:
        isr &= ~(v & 0x7f);
        break;
    case 8:
        remote = (remote & 0xff00) | uint8_t(v);
        break;
    case 9:
        remote = (remote & 255) | uint16_t(v << 8);
        break;
    case 10:
        remoteCount = (remoteCount & 0xff00) | uint8_t(v);
        break;
    case 11:
        remoteCount = (remoteCount & 255) | uint16_t(v << 8);
        break;
    case 12:
        rcr = v;
        break;
    case 13:
        tcr = v;
        break;
    case 14:
        dcr = v;
        break;
    case 15:
        imr = v & 0x7f;
        break;
    }
}
void NE2000::transmit() {
    unsigned address = unsigned(transmitPage) << 8;
    if (transmitCount < 14 || transmitCount > 1518 || address < 0x4000 ||
        address + transmitCount > 0x8000) {
        transmitStatus = 8;
        isr |= 8;
        return;
    }
    std::vector<uint8_t> packet(memory.begin() + address, memory.begin() + address + transmitCount);
    packet.resize(std::max<size_t>(60, packet.size()), 0);
    if (tcr & 6)
        receive(packet.data(), packet.size());
    else if (outgoing.size() < 64)
        outgoing.push_back(std::move(packet));
    transmitStatus = 1;
    isr |= 2;
    ++transmitted;
}
bool NE2000::receive(const uint8_t *p, size_t n) {
    if ((command & 1) || !(command & 2) || (rcr & 0x20) || n < 14 || n > 1518)
        return false;
    bool broadcast = std::all_of(p, p + 6, [](uint8_t b) { return b == 255; });
    bool group = p[0] & 1;
    if (!(rcr & 0x10)) {
        if (broadcast) {
            if (!(rcr & 4))
                return false;
        } else if (group) {
            if (!(rcr & 8))
                return false;
            uint32_t crc = 0xffffffff;
            for (unsigned i = 0; i < 6; ++i)
                for (unsigned bit = 0; bit < 8; ++bit) {
                    bool carry = ((crc >> 31) ^ (p[i] >> bit)) & 1;
                    crc <<= 1;
                    if (carry)
                        crc ^= 0x04c11db7;
                }
            unsigned index = crc >> 26;
            if (!(multicast[index >> 3] & (1 << (index & 7))))
                return false;
        } else if (std::memcmp(p, physical.data(), 6))
            return false;
    }
    if (n < 60 && !(rcr & 2))
        return false;
    if (start < 0x40 || stop > 0x80 || start >= stop || current < start || current >= stop ||
        boundary < start || boundary >= stop) {
        ++dropped;
        return false;
    }
    size_t bytes = std::max<size_t>(60, n);
    unsigned pages = unsigned((bytes + 8 + 255) / 256);
    unsigned next = current;
    for (unsigned i = 0; i < pages; ++i) {
        if (++next == stop)
            next = start;
        if (next == boundary) {
            isr |= 0x10;
            receiveStatus = 0x10;
            ++dropped;
            if (++tally[2] == 0x80)
                isr |= 0x20;
            return false;
        }
    }
    uint16_t address = uint16_t(current) << 8;
    auto put = [&](uint8_t v) {
        memory[address++] = v;
        if (address == uint16_t(stop) * 256)
            address = uint16_t(start) * 256;
    };
    receiveStatus = 1 | (group ? 0x20 : 0);
    put(receiveStatus);
    put(next);
    put((bytes + 4) & 255);
    put((bytes + 4) >> 8);
    for (size_t i = 0; i < bytes; ++i)
        put(i < n ? p[i] : 0);
    uint32_t crc = 0xffffffff;
    for (size_t i = 0; i < bytes; ++i) {
        crc ^= i < n ? p[i] : 0;
        for (unsigned b = 0; b < 8; ++b)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320 : 0);
    }
    crc = ~crc;
    for (unsigned i = 0; i < 4; ++i)
        put(crc >> (8 * i));
    current = next;
    local = address;
    isr |= 1;
    ++received;
    return true;
}
} // namespace pc
