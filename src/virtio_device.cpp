#include "virtio_device.hpp"
#include <algorithm>
#include <bitset>
#include <stdexcept>

namespace pc {
VirtioDevice::VirtioDevice(std::span<uint8_t> r, uint64_t base, uint32_t deviceId, unsigned count,
                           uint64_t bits, Interrupt irq)
    : ram(r), ramBase(base), features(bits | (1ull << 32)), id(deviceId), queues(count),
      interrupt(std::move(irq)) {
    if (!count || count > 256)
        throw std::runtime_error("Invalid virtio queue count");
}
std::span<uint8_t> VirtioDevice::memory(uint64_t address, uint64_t length) const {
    if (address < ramBase || length > ram.size() || address - ramBase > ram.size() - length)
        throw std::runtime_error("Virtio DMA outside guest RAM");
    return ram.subspan(address - ramBase, length);
}
uint64_t VirtioDevice::get(uint64_t address, unsigned width) const {
    auto bytes = memory(address, width);
    uint64_t value = 0;
    for (unsigned i = 0; i < width; ++i)
        value |= uint64_t(bytes[i]) << (i * 8);
    return value;
}
void VirtioDevice::put(uint64_t address, uint64_t value, unsigned width) {
    auto bytes = memory(address, width);
    for (unsigned i = 0; i < width; ++i)
        bytes[i] = uint8_t(value >> (i * 8));
}
void VirtioDevice::reset() {
    ++resetCount;
    featureSelect = driverSelect = queueSelect = status = interruptStatus = generation = 0;
    driverFeatures = 0;
    std::fill(queues.begin(), queues.end(), Queue{});
    failure.clear();
    deviceReset();
    interrupt(false, 0);
}
void VirtioDevice::broken(const std::string &reason) {
    failure = reason;
    status |= 64;
    configurationChanged();
}
void VirtioDevice::configurationChanged() {
    ++generation;
    interruptStatus |= 2;
    interrupt(true, lastInterruptQueue);
}
uint64_t VirtioDevice::read(unsigned offset, unsigned width) const {
    if ((width != 1 && width != 2 && width != 4 && width != 8) || offset % width)
        throw std::runtime_error("Invalid virtio register access");
    if (offset >= 0x100 && offset < 0x140 && width <= 0x140 - offset)
        return configRead(offset - 0x100, width);
    if (width != 4)
        throw std::runtime_error("Virtio controls require 32-bit access");
    const auto *q = queueSelect < queues.size() ? &queues[queueSelect] : nullptr;
    switch (offset) {
    case 0:
        return 0x74726976;
    case 4:
        return 2;
    case 8:
        return id;
    case 12:
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
        return q ? 256 : 0;
    case 0x38:
        return q ? q->size : 0;
    case 0x44:
        return q && q->ready;
    case 0x60:
        return interruptStatus;
    case 0x70:
        return status;
    case 0x80:
        return q ? uint32_t(q->descriptors) : 0;
    case 0x84:
        return q ? q->descriptors >> 32 : 0;
    case 0x90:
        return q ? uint32_t(q->available) : 0;
    case 0x94:
        return q ? q->available >> 32 : 0;
    case 0xa0:
        return q ? uint32_t(q->used) : 0;
    case 0xa4:
        return q ? q->used >> 32 : 0;
    case 0xfc:
        return generation;
    default:
        return 0;
    }
}
void VirtioDevice::validate(const Queue &q) const {
    if (!q.size || q.size > 256 || (q.size & (q.size - 1)) || q.descriptors % 16 ||
        q.available % 2 || q.used % 4)
        throw std::runtime_error("Invalid virtio split queue geometry");
    memory(q.descriptors, uint64_t(q.size) * 16);
    memory(q.available, 6 + uint64_t(q.size) * 2);
    memory(q.used, 6 + uint64_t(q.size) * 8);
}
void VirtioDevice::write(unsigned offset, uint64_t raw, unsigned width) {
    if ((width != 1 && width != 2 && width != 4 && width != 8) || offset % width)
        throw std::runtime_error("Invalid virtio register access");
    if (offset >= 0x100 && offset < 0x140 && width <= 0x140 - offset) {
        configWrite(offset - 0x100, raw, width);
        return;
    }
    if (width != 4)
        throw std::runtime_error("Virtio controls require 32-bit access");
    uint32_t value = uint32_t(raw);
    if (offset == 0x70 && !value) {
        reset();
        return;
    }
    if (offset == 0x64) {
        interruptStatus &= ~value;
        interrupt(interruptStatus != 0, lastInterruptQueue);
        return;
    }
    if (status & 192)
        return;
    auto *q = queueSelect < queues.size() ? &queues[queueSelect] : nullptr;
    auto address = [&](uint64_t &target, bool high) {
        if (q->ready) {
            broken("Changing an active virtqueue");
            return;
        }
        target = high ? (target & 0xffffffffull) | (uint64_t(value) << 32)
                      : (target & 0xffffffff00000000ull) | value;
    };
    switch (offset) {
    case 0x14:
        featureSelect = value;
        break;
    case 0x24:
        driverSelect = value;
        break;
    case 0x20:
        if (status & 8) {
            broken("Changing negotiated virtio features");
            break;
        }
        if (driverSelect < 2) {
            auto shift = driverSelect * 32;
            driverFeatures =
                (driverFeatures & ~(0xffffffffull << shift)) | (uint64_t(value) << shift);
        }
        break;
    case 0x30:
        queueSelect = value;
        break;
    case 0x38:
        if (!q)
            break;
        if (q->ready)
            broken("Changing active virtqueue size");
        else
            q->size = value;
        break;
    case 0x44:
        if (!q)
            break;
        if (value > 1) {
            broken("Invalid QueueReady");
            break;
        }
        if (value) {
            try {
                validate(*q);
                q->ready = true;
            } catch (const std::runtime_error &e) {
                broken(e.what());
            }
        } else if (q->ready)
            broken("Virtqueue reset was not negotiated");
        break;
    case 0x50:
        if (value < queues.size() && running())
            notified(value);
        break;
    case 0x70:
        status = value & 255;
        if ((status & 8) && ((driverFeatures & ~features) || !(driverFeatures & (1ull << 32))))
            status &= ~8u;
        if ((status & 4) && !(status & 8))
            broken("DRIVER_OK without modern features");
        break;
    case 0x80:
        if (q)
            address(q->descriptors, false);
        break;
    case 0x84:
        if (q)
            address(q->descriptors, true);
        break;
    case 0x90:
        if (q)
            address(q->available, false);
        break;
    case 0x94:
        if (q)
            address(q->available, true);
        break;
    case 0xa0:
        if (q)
            address(q->used, false);
        break;
    case 0xa4:
        if (q)
            address(q->used, true);
        break;
    default:
        break;
    }
}
VirtioDevice::Chain VirtioDevice::chain(const Queue &q, uint16_t index) const {
    Chain result;
    std::bitset<256> visited;
    bool writing = false;
    for (;;) {
        if (index >= q.size || visited[index])
            throw std::runtime_error("Invalid/cyclic virtio descriptor chain: index=" +
                                     std::to_string(index) + " size=" + std::to_string(q.size) +
                                     " descriptors=" + std::to_string(q.descriptors));
        visited.set(index);
        auto descriptor = q.descriptors + uint64_t(index) * 16;
        auto flags = get(descriptor + 12, 2);
        if (flags & ~3u)
            throw std::runtime_error("Unsupported virtio descriptor flags");
        bool writable = (flags & 2) != 0;
        if (writing && !writable)
            throw std::runtime_error("Readable buffer follows writable virtio buffer");
        writing |= writable;
        auto bytes = memory(get(descriptor, 8), get(descriptor + 8, 4));
        result.segments.push_back({bytes, writable});
        (writable ? result.writable : result.readable) += bytes.size();
        if (result.readable + result.writable > 16 * 1024 * 1024)
            throw std::runtime_error("Virtio descriptor chain exceeds processing bound");
        if (!(flags & 1))
            break;
        index = uint16_t(get(descriptor + 14, 2));
    }
    return result;
}
std::vector<uint8_t> VirtioDevice::Chain::input(size_t maximum) const {
    if (readable > maximum)
        throw std::runtime_error("Virtio request exceeds device bound");
    std::vector<uint8_t> result;
    result.reserve(readable);
    for (const auto &s : segments)
        if (!s.writable)
            result.insert(result.end(), s.bytes.begin(), s.bytes.end());
    return result;
}
void VirtioDevice::Chain::output(std::span<const uint8_t> bytes) const {
    if (bytes.size() > writable)
        throw std::runtime_error("Virtio response buffer too small");
    for (const auto &s : segments)
        if (s.writable) {
            auto count = std::min(s.bytes.size(), bytes.size());
            std::copy_n(bytes.begin(), count, s.bytes.begin());
            bytes = bytes.subspan(count);
            if (bytes.empty())
                break;
        }
}
void VirtioDevice::process(unsigned queue) {
    if (!running() || queue >= queues.size() || !queues[queue].ready)
        return;
    auto &q = queues[queue];
    try {
        validate(q);
        auto next = uint16_t(get(q.available + 2, 2));
        if (uint16_t(next - q.nextAvailable) > q.size)
            throw std::runtime_error("Virtio available ring overrun");
        bool completed = false;
        while (q.nextAvailable != next) {
            auto head = uint16_t(get(q.available + 4 + uint64_t(q.nextAvailable % q.size) * 2, 2));
            auto buffers = chain(q, head);
            auto length = handle(queue, buffers);
            if (!length)
                break;
            if (*length > buffers.writable)
                throw std::runtime_error("Invalid virtio used length");
            auto entry = q.used + 4 + uint64_t(q.nextUsed % q.size) * 8;
            put(entry, head, 4);
            put(entry + 4, *length, 4);
            ++q.nextAvailable;
            ++q.nextUsed;
            ++completions;
            put(q.used + 2, q.nextUsed, 2);
            completed = true;
        }
        if (completed && !(get(q.available, 2) & 1)) {
            lastInterruptQueue = queue;
            interruptStatus |= 1;
            interrupt(true, queue);
        }
    } catch (const std::runtime_error &e) {
        broken("queue=" + std::to_string(queue) + " avail=" + std::to_string(q.nextAvailable) +
               " used=" + std::to_string(q.nextUsed) + " " + e.what());
    }
}
} // namespace pc
