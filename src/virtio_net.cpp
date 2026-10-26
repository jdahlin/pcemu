#include "virtio_net.hpp"
#include <algorithm>
#include <bitset>
#include <cstring>
#include <stdexcept>
namespace pc {
VirtioNet::VirtioNet(std::span<uint8_t> r, uint64_t base, std::function<void(bool)> irq)
    : ram(r), ramBase(base), interrupt(std::move(irq)) {}
uint8_t *VirtioNet::memory(uint64_t address, uint64_t length) const {
    if (address < ramBase || length > ram.size() || address - ramBase > ram.size() - length)
        throw std::runtime_error("virtio-net DMA outside guest RAM");
    return ram.data() + (address - ramBase);
}
uint64_t VirtioNet::get(uint64_t address, unsigned width) const {
    auto *p = memory(address, width);
    uint64_t value = 0;
    for (unsigned i = 0; i < width; ++i)
        value |= uint64_t(p[i]) << (i * 8);
    return value;
}
void VirtioNet::put(uint64_t address, uint64_t value, unsigned width) {
    auto *p = memory(address, width);
    for (unsigned i = 0; i < width; ++i)
        p[i] = value >> (i * 8);
}
uint64_t VirtioNet::read(unsigned offset, unsigned width) const {
    if ((width != 1 && width != 2 && width != 4 && width != 8) || offset % width)
        throw std::runtime_error("Invalid virtio-net register read");
    if (offset >= 0x100 && offset <= 0x13f && width <= 0x140 - offset) {
        std::array<uint8_t, 64> config{0x52, 0x54, 0, 0x12, 0x34, 0x56, 1, 0, 0, 0, 0xdc, 5};
        uint64_t value = 0;
        for (unsigned i = 0; i < width; ++i)
            value |= uint64_t(config[offset - 0x100 + i]) << (8 * i);
        return value;
    }
    if (width != 4)
        throw std::runtime_error("virtio-net control read requires 32 bits");
    const Queue empty{};
    const auto &q = queueSelect < 2 ? queues[queueSelect] : empty;
    switch (offset) {
    case 0:
        return 0x74726976;
    case 4:
        return 2;
    case 8:
        return 1;
    case 0xc:
        return 0x5043454d;
    case 0x10:
        return featureSelect < 2 ? uint32_t(features >> (32 * featureSelect)) : 0;
    case 0x14:
        return featureSelect;
    case 0x20:
        return driverSelect < 2 ? uint32_t(driverFeatures >> (32 * driverSelect)) : 0;
    case 0x24:
        return driverSelect;
    case 0x30:
        return queueSelect;
    case 0x34:
        return queueSelect < 2 ? 256 : 0;
    case 0x38:
        return queueSelect < 2 ? q.size : 0;
    case 0x44:
        return q.ready;
    case 0x60:
        return isr;
    case 0x70:
        return status;
    case 0x80:
        return uint32_t(q.descriptors);
    case 0x84:
        return q.descriptors >> 32;
    case 0x90:
        return uint32_t(q.available);
    case 0x94:
        return q.available >> 32;
    case 0xa0:
        return uint32_t(q.used);
    case 0xa4:
        return q.used >> 32;
    default:
        return 0;
    }
}
void VirtioNet::broken(const std::string &reason) {
    failure = reason;
    status |= 64;
    isr |= 2;
    interrupt(true);
}
void VirtioNet::validate(const Queue &q) const {
    if (!q.size || q.size > 256 || (q.size & (q.size - 1)) || q.descriptors % 16 ||
        q.available % 2 || q.used % 4)
        throw std::runtime_error("Invalid virtio-net split queue geometry");
    memory(q.descriptors, q.size * 16);
    memory(q.available, 6 + q.size * 2);
    memory(q.used, 6 + q.size * 8);
}
void VirtioNet::write(unsigned offset, uint64_t value64, unsigned width) {
    if (width != 4 || offset % 4)
        throw std::runtime_error("Invalid virtio-net register write");
    uint32_t value = value64;
    if (offset == 0x70 && !value) {
        ++resetCount;
        queues = {};
        featureSelect = driverSelect = queueSelect = status = isr = 0;
        driverFeatures = 0;
        failure.clear();
        outgoing.clear();
        interrupt(false);
        return;
    }
    if (status & (64 | 128))
        return;
    try {
        if (offset == 0x14) {
            featureSelect = value;
            return;
        }
        if (offset == 0x24) {
            driverSelect = value;
            return;
        }
        if (offset == 0x20) {
            if (status & 8)
                throw std::runtime_error("Changing negotiated virtio-net features");
            if (driverSelect < 2) {
                unsigned shift = driverSelect * 32;
                driverFeatures =
                    (driverFeatures & ~(0xffffffffull << shift)) | (uint64_t(value) << shift);
            }
            return;
        }
        if (offset == 0x30) {
            queueSelect = value;
            return;
        }
        if (offset == 0x64) {
            isr &= ~value;
            interrupt(isr != 0);
            return;
        }
        if (offset == 0x70) {
            status = value & 255;
            if ((status & 8) && ((driverFeatures & ~features) || !(driverFeatures & (1ull << 32))))
                status &= ~8u;
            if ((status & 4) && !(status & 8))
                throw std::runtime_error("virtio-net DRIVER_OK without FEATURES_OK");
            return;
        }
        if (offset == 0x50) {
            if (value == 1)
                transmit();
            return;
        }
        if (queueSelect >= 2)
            return;
        auto &q = queues[queueSelect];
        if (offset == 0x44) {
            if (value > 1)
                throw std::runtime_error("Invalid virtio-net queue enable");
            if (value) {
                validate(q);
                q.ready = 1;
            } else {
                q.ready = q.last = q.index = 0;
            }
            return;
        }
        if (offset == 0x38 || (offset >= 0x80 && offset <= 0xa4)) {
            if (q.ready)
                throw std::runtime_error("Changing active virtio-net queue");
            if (offset == 0x38)
                q.size = value;
            else if ((offset & 15) == 0 || (offset & 15) == 4) {
                auto &address = offset < 0x90   ? q.descriptors
                                : offset < 0xa0 ? q.available
                                                : q.used;
                address = (offset & 4) ? (address & 0xffffffffull) | (uint64_t(value) << 32)
                                       : (address & 0xffffffff00000000ull) | value;
            }
        }
    } catch (const std::runtime_error &e) {
        broken(e.what());
    }
}
std::vector<VirtioNet::Buffer> VirtioNet::chain(const Queue &q, uint16_t head,
                                                bool writable) const {
    std::bitset<256> seen;
    std::vector<Buffer> buffers;
    size_t total = 0;
    for (;;) {
        if (head >= q.size || seen[head])
            throw std::runtime_error("Invalid/cyclic virtio-net descriptor");
        seen.set(head);
        auto d = q.descriptors + head * 16;
        auto address = get(d, 8);
        auto length = uint32_t(get(d + 8, 4));
        auto flags = get(d + 12, 2);
        if ((flags & ~3u) || bool(flags & 2) != writable)
            throw std::runtime_error("Invalid virtio-net descriptor direction/flags");
        memory(address, length);
        total += length;
        if (total > 65536)
            throw std::runtime_error("Oversized virtio-net buffer chain");
        buffers.push_back({address, length});
        if (!(flags & 1))
            return buffers;
        head = get(d + 14, 2);
    }
}
VirtioNet::Bytes VirtioNet::gather(const std::vector<Buffer> &buffers) const {
    Bytes data;
    for (auto b : buffers) {
        auto *p = memory(b.address, b.length);
        data.insert(data.end(), p, p + b.length);
    }
    return data;
}
void VirtioNet::scatter(const std::vector<Buffer> &buffers, const Bytes &packet) {
    size_t capacity = 0;
    for (auto b : buffers)
        capacity += b.length;
    if (capacity < packet.size())
        throw std::runtime_error("Short virtio-net receive buffer");
    size_t offset = 0;
    for (auto b : buffers) {
        auto size = std::min<size_t>(b.length, packet.size() - offset);
        std::memcpy(memory(b.address, size), packet.data() + offset, size);
        offset += size;
        if (offset == packet.size())
            break;
    }
}
uint16_t VirtioNet::pending(const Queue &q) const {
    auto count = uint16_t(get(q.available + 2, 2) - q.last);
    if (count > q.size)
        throw std::runtime_error("virtio-net available ring overrun");
    return count;
}
void VirtioNet::complete(Queue &q, uint16_t head, uint32_t length) {
    auto entry = q.used + 4 + (q.index % q.size) * 8;
    put(entry, head, 4);
    put(entry + 4, length, 4);
    ++q.index;
    ++q.last;
    put(q.used + 2, q.index, 2);
    if (driverFeatures & (1ull << 29))
        put(q.used + 4 + q.size * 8, q.last, 2);
}
void VirtioNet::signal(Queue &q, uint16_t previous) {
    bool send;
    if (driverFeatures & (1ull << 29)) {
        auto event = uint16_t(get(q.available + 4 + q.size * 2, 2));
        send = uint16_t(q.index - event - 1) < uint16_t(q.index - previous);
    } else
        send = !(get(q.available, 2) & 1);
    if (send && previous != q.index) {
        signaledQueue = unsigned(&q - queues.data());
        isr |= 1;
        interrupt(true);
    }
}
void VirtioNet::transmit() {
    auto &q = queues[1];
    if (!q.ready || (status & 15) != 15 || (status & (64 | 128)))
        return;
    validate(q);
    auto previous = q.index;
    auto count = pending(q);
    while (count--) {
        auto head = uint16_t(get(q.available + 4 + (q.last % q.size) * 2, 2));
        auto packet = gather(chain(q, head, false));
        if (packet.size() < 26 || packet.size() > 12 + 1518 || packet[1])
            throw std::runtime_error("Unsupported virtio-net packet size/segmentation");
        if (packet[0] & 1) {
            if (!(driverFeatures & 1))
                throw std::runtime_error("Unnegotiated virtio-net checksum offload");
            unsigned start = packet[6] | (packet[7] << 8), offset = packet[8] | (packet[9] << 8);
            if (start > packet.size() - 12 || offset + 2 > packet.size() - 12 - start)
                throw std::runtime_error("Invalid virtio-net checksum range");
            uint32_t sum = 0;
            for (size_t i = 12 + start; i < packet.size(); i += 2)
                sum += (unsigned(packet[i]) << 8) | (i + 1 < packet.size() ? packet[i + 1] : 0);
            while (sum >> 16)
                sum = (sum & 65535) + (sum >> 16);
            auto at = 12 + start + offset;
            packet[at] = (~sum) >> 8;
            packet[at + 1] = ~sum;
        }
        if (outgoing.size() < 256) {
            outgoing.emplace_back(packet.begin() + 12, packet.end());
            ++transmitted;
        } else
            ++dropped;
        complete(q, head, 0);
    }
    signal(q, previous);
}
void VirtioNet::receive(const uint8_t *data, size_t length) {
    auto &q = queues[0];
    if (!q.ready || (status & 15) != 15 || (status & (64 | 128)) || length < 14 || length > 1518) {
        ++dropped;
        return;
    }
    try {
        validate(q);
        if (!pending(q)) {
            ++dropped;
            return;
        }
        auto head = uint16_t(get(q.available + 4 + (q.last % q.size) * 2, 2));
        auto buffers = chain(q, head, true);
        Bytes packet(12, 0);
        packet.insert(packet.end(), data, data + length);
        scatter(buffers, packet);
        auto previous = q.index;
        complete(q, head, packet.size());
        signal(q, previous);
        ++received;
    } catch (const std::runtime_error &e) {
        broken(e.what());
    }
}
} // namespace pc
