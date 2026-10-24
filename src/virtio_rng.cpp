#include "virtio_rng.hpp"
#include <algorithm>
#include <array>
#include <bitset>
#include <cstdlib>
#include <stdexcept>
namespace pc {
namespace {
uint64_t little(const uint8_t *p, unsigned width) {
    uint64_t value = 0;
    for (unsigned i = 0; i < width; ++i) value |= uint64_t(p[i]) << (i * 8);
    return value;
}
}
VirtioRng::VirtioRng(std::span<uint8_t> r, uint64_t base,
                     std::function<void(bool)> irq, Entropy source)
    : ram(r), ramBase(base), interrupt(std::move(irq)), entropy(std::move(source)) {
    if (!entropy) entropy = [](std::span<uint8_t> bytes) {
        arc4random_buf(bytes.data(), bytes.size());
    };
}
uint8_t *VirtioRng::memory(uint64_t address, uint64_t length) const {
    if (address < ramBase || length > ram.size() || address - ramBase > ram.size() - length)
        throw std::runtime_error("virtio DMA outside guest RAM");
    return ram.data() + (address - ramBase);
}
uint64_t VirtioRng::get(uint64_t address, unsigned width) const {
    return little(memory(address, width), width);
}
void VirtioRng::put(uint64_t address, uint64_t value, unsigned width) {
    auto *p = memory(address, width);
    for (unsigned i = 0; i < width; ++i) p[i] = uint8_t(value >> (i * 8));
}
void VirtioRng::reset() {
    featureSelect = driverSelect = queueSelect = queueSize = ready = status = interruptStatus = 0;
    driverFeatures = descriptors = available = used = 0;
    lastAvailable = usedIndex = 0;
    failure.clear();
    interrupt(false);
}
uint64_t VirtioRng::read(unsigned offset, unsigned width) const {
    if ((width != 1 && width != 2 && width != 4 && width != 8) || offset % width)
        throw std::runtime_error("Invalid virtio MMIO read width/alignment");
    if (offset >= 0x100 && offset < 0x140 && width <= 0x140 - offset) return 0;
    if (width != 4) throw std::runtime_error("virtio control registers require 32-bit access");
    switch (offset) {
    case 0: return 0x74726976;
    case 4: return 2;
    case 8: return 4; // entropy device
    case 0xc: return 0x5043454d;
    case 0x10: return featureSelect < 2 ? uint32_t(features >> (32 * featureSelect)) : 0;
    case 0x14: return featureSelect;
    case 0x20: return driverSelect < 2 ? uint32_t(driverFeatures >> (32 * driverSelect)) : 0;
    case 0x24: return driverSelect;
    case 0x30: return queueSelect;
    case 0x34: return queueSelect == 0 ? 256 : 0;
    case 0x38: return queueSelect == 0 ? queueSize : 0;
    case 0x44: return queueSelect == 0 ? ready : 0;
    case 0x60: return interruptStatus;
    case 0x70: return status;
    case 0x80: return uint32_t(descriptors);
    case 0x84: return descriptors >> 32;
    case 0x90: return uint32_t(available);
    case 0x94: return available >> 32;
    case 0xa0: return uint32_t(used);
    case 0xa4: return used >> 32;
    case 0xfc: return 0;
    default: return 0; // reserved/read-only config fields
    }
}
void VirtioRng::broken(const std::string &reason) {
    failure = reason;
    status |= 64; // DEVICE_NEEDS_RESET
    interruptStatus |= 2;
    interrupt(true);
}
void VirtioRng::validateQueue() const {
    if (!queueSize || queueSize > 256 || (queueSize & (queueSize - 1)) ||
        descriptors % 16 || available % 2 || used % 4)
        throw std::runtime_error("Invalid virtio split queue geometry");
    memory(descriptors, uint64_t(queueSize) * 16);
    memory(available, 6 + uint64_t(queueSize) * 2);
    memory(used, 6 + uint64_t(queueSize) * 8);
}
void VirtioRng::write(unsigned offset, uint64_t value64, unsigned width) {
    if (width != 4 || offset % 4)
        throw std::runtime_error("virtio registers require aligned 32-bit writes");
    uint32_t value = uint32_t(value64);
    if (offset == 0x70 && value == 0) { reset(); return; }
    if (status & (64 | 128)) return;
    auto address = [&](uint64_t &target, bool high) {
        if (ready) { broken("Changing an active virtqueue"); return; }
        target = high ? (target & 0xffffffffull) | (uint64_t(value) << 32)
                      : (target & 0xffffffff00000000ull) | value;
    };
    switch (offset) {
    case 0x14: featureSelect = value; break;
    case 0x24: driverSelect = value; break;
    case 0x20:
        if (status & 8) { broken("Changing negotiated virtio features"); break; }
        if (driverSelect < 2) {
            const auto shift = 32 * driverSelect;
            driverFeatures = (driverFeatures & ~(0xffffffffull << shift)) | (uint64_t(value) << shift);
        }
        break;
    case 0x30: queueSelect = value; break;
    case 0x38:
        if (queueSelect) break;
        if (ready) { broken("Changing active virtqueue size"); break; }
        queueSize = value; break;
    case 0x44:
        if (queueSelect) break;
        if (value > 1) { broken("Invalid QueueReady"); break; }
        if (value) {
            try { validateQueue(); ready = 1; }
            catch (const std::runtime_error &e) { broken(e.what()); }
        } else { ready = 0; lastAvailable = usedIndex = 0; }
        break;
    case 0x50: if (value == 0) notify(); break;
    case 0x64: interruptStatus &= ~value; interrupt(interruptStatus != 0); break;
    case 0x70:
        status = value & 0xff;
        if ((status & 8) && ((driverFeatures & ~features) || !(driverFeatures & (1ull << 32))))
            status &= ~8u;
        if ((status & 4) && !(status & 8)) broken("DRIVER_OK without negotiated modern features");
        break;
    case 0x80: if (!queueSelect) address(descriptors, false); break;
    case 0x84: if (!queueSelect) address(descriptors, true); break;
    case 0x90: if (!queueSelect) address(available, false); break;
    case 0x94: if (!queueSelect) address(available, true); break;
    case 0xa0: if (!queueSelect) address(used, false); break;
    case 0xa4: if (!queueSelect) address(used, true); break;
    default: break; // writes to read-only/reserved registers
    }
}
uint32_t VirtioRng::request(uint16_t index) {
    std::bitset<256> visited;
    std::vector<std::span<uint8_t>> buffers;
    uint64_t capacity = 0;
    for (;;) {
        if (index >= queueSize || visited[index])
            throw std::runtime_error("Invalid/cyclic entropy descriptor chain");
        visited.set(index);
        auto descriptor = descriptors + uint64_t(index) * 16;
        auto address = get(descriptor, 8);
        uint32_t length = uint32_t(get(descriptor + 8, 4));
        auto flags = get(descriptor + 12, 2);
        if (!(flags & 2) || (flags & ~3u))
            throw std::runtime_error("Entropy descriptors must be writable, direct buffers");
        buffers.emplace_back(memory(address, length), length);
        capacity += length;
        if (!(flags & 1)) break;
        index = uint16_t(get(descriptor + 14, 2));
    }
    if (!capacity) throw std::runtime_error("Empty entropy request");
    // Validate the entire chain before any write. Partial fills are permitted;
    // cap CPU/entropy work even when a guest supplies very large buffers.
    uint32_t supplied = uint32_t(std::min<uint64_t>(capacity, 65536));
    size_t remaining = supplied;
    for (auto buffer : buffers) {
        auto length = std::min(buffer.size(), remaining);
        if (length) entropy(buffer.first(length));
        remaining -= length;
        if (!remaining) break;
    }
    bytesSupplied += supplied;
    return supplied;
}
void VirtioRng::notify() {
    if (!ready || (status & 15) != 15 || (status & (64 | 128))) return;
    try {
        validateQueue();
        uint16_t next = uint16_t(get(available + 2, 2));
        if (uint16_t(next - lastAvailable) > queueSize)
            throw std::runtime_error("Virtio available ring overrun");
        bool completed = lastAvailable != next;
        while (lastAvailable != next) {
            auto head = uint16_t(get(available + 4 + uint64_t(lastAvailable % queueSize) * 2, 2));
            auto length = request(head);
            auto entry = used + 4 + uint64_t(usedIndex % queueSize) * 8;
            put(entry, head, 4); put(entry + 4, length, 4);
            ++lastAvailable; ++usedIndex;
            put(used + 2, usedIndex, 2);
        }
        if (completed && !(get(available, 2) & 1)) {
            interruptStatus |= 1;
            interrupt(true);
        }
    } catch (const std::runtime_error &e) { broken(e.what()); }
}
} // namespace pc
