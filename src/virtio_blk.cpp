#include "virtio_blk.hpp"
#include <algorithm>
#include <array>
#include <bitset>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
namespace pc {
namespace {
void traceBlock(const void *device, const char *operation, unsigned offset, uint64_t value, unsigned width) {
    static const bool enabled = std::getenv("PCEMU_TRACE_VIRTIO_BLK") != nullptr;
    static unsigned remaining = 4096;
    if (enabled && remaining) {
        --remaining;
        std::fprintf(stderr, "virtio-blk[%p] %s offset=%#x value=%#llx width=%u\n",
                     device, operation, offset, static_cast<unsigned long long>(value), width);
    }
}
// EDK II does not negotiate SIZE_MAX and submits whole EFI images in one
// descriptor. Bound aggregate I/O, but stream it through a small host buffer.
constexpr uint64_t maxRequest = 1024ull * 1024 * 1024;
constexpr size_t transferChunk = 128 * 1024;
uint64_t little(const uint8_t *p, unsigned width) {
    uint64_t value = 0;
    for (unsigned i = 0; i < width; ++i) value |= uint64_t(p[i]) << (i * 8);
    return value;
}
}
VirtioBlock::VirtioBlock(Disk &d, std::span<uint8_t> r, uint64_t base,
                         std::function<void(bool)> irq)
    : disk(d), ram(r), ramBase(base), interrupt(std::move(irq)) {}
uint8_t *VirtioBlock::memory(uint64_t address, uint64_t length) const {
    if (address < ramBase || length > ram.size() || address - ramBase > ram.size() - length)
        throw std::runtime_error("virtio DMA outside guest RAM");
    return ram.data() + (address - ramBase);
}
uint64_t VirtioBlock::get(uint64_t address, unsigned width) const {
    return little(memory(address, width), width);
}
void VirtioBlock::put(uint64_t address, uint64_t value, unsigned width) {
    auto *p = memory(address, width);
    for (unsigned i = 0; i < width; ++i) p[i] = uint8_t(value >> (i * 8));
}
void VirtioBlock::reset() {
    featureSelect = driverSelect = queueSelect = queueSize = ready = status = interruptStatus = 0;
    driverFeatures = descriptors = available = used = 0;
    lastAvailable = usedIndex = 0;
    failure.clear();
    interrupt(false);
}
uint64_t VirtioBlock::read(unsigned offset, unsigned width) const {
    if ((width != 1 && width != 2 && width != 4 && width != 8) || offset % width)
        throw std::runtime_error("Invalid virtio MMIO read width/alignment");
    if (offset >= 0x100 && offset <= 0x13c && width <= 0x140 - offset) {
        std::array<uint8_t, 64> config{};
        for (unsigned i = 0; i < 8; ++i) config[i] = uint8_t(disk.sectors >> (8 * i));
        config[10] = 2; config[12] = 126; // size_max=128 KiB, seg_max=126
        config[20] = 0; config[21] = 2; // blk_size = 512
        return little(config.data() + offset - 0x100, width);
    }
    if (width != 4) throw std::runtime_error("virtio control registers require 32-bit access");
    switch (offset) {
    case 0: return 0x74726976;
    case 4: return 2;
    case 8: return 2; // block device
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
void VirtioBlock::broken(const std::string &reason) {
    traceBlock(this, reason.c_str(), 0x70, status, 4);
    failure = reason;
    status |= 64; // DEVICE_NEEDS_RESET
    interruptStatus |= 2;
    interrupt(true);
}
void VirtioBlock::validateQueue() const {
    if (!queueSize || queueSize > 256 || (queueSize & (queueSize - 1)) ||
        descriptors % 16 || available % 2 || used % 4)
        throw std::runtime_error("Invalid virtio split queue geometry");
    memory(descriptors, uint64_t(queueSize) * 16);
    memory(available, 6 + uint64_t(queueSize) * 2);
    memory(used, 6 + uint64_t(queueSize) * 8);
}
void VirtioBlock::write(unsigned offset, uint64_t value64, unsigned width) {
    if (width != 4 || offset % 4)
        throw std::runtime_error("virtio registers require aligned 32-bit writes");
    traceBlock(this, "write", offset, value64, width);
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
std::vector<VirtioBlock::Buffer> VirtioBlock::chain(uint16_t index) const {
    std::bitset<256> visited;
    std::vector<Buffer> buffers;
    bool writing = false;
    for (;;) {
        if (index >= queueSize || visited[index]) throw std::runtime_error("Invalid/cyclic virtio descriptor chain");
        visited.set(index);
        const auto desc = descriptors + uint64_t(index) * 16;
        uint64_t address = get(desc, 8);
        uint32_t length = uint32_t(get(desc + 8, 4));
        uint16_t flags = uint16_t(get(desc + 12, 2));
        if (flags & ~3u) throw std::runtime_error("Unsupported virtio descriptor flags");
        memory(address, length);
        bool writable = flags & 2;
        if (writing && !writable) throw std::runtime_error("Virtio readable buffer follows writable buffer");
        writing |= writable;
        buffers.push_back({address, length, writable});
        if (!(flags & 1)) return buffers;
        index = uint16_t(get(desc + 14, 2));
    }
}
void VirtioBlock::copy(const std::vector<Buffer> &buffers, bool writable, size_t skip,
                       std::span<uint8_t> bytes, bool toGuest) {
    for (const auto &b : buffers) {
        if (b.writable != writable) continue;
        if (skip >= b.length) { skip -= b.length; continue; }
        size_t length = std::min<size_t>(b.length - skip, bytes.size());
        auto *pointer = memory(b.address + skip, length);
        if (toGuest) std::memcpy(pointer, bytes.data(), length);
        else std::memcpy(bytes.data(), pointer, length);
        bytes = bytes.subspan(length); skip = 0;
        if (bytes.empty()) return;
    }
    if (!bytes.empty()) throw std::runtime_error("Short virtio scatter/gather chain");
}
uint32_t VirtioBlock::request(uint16_t head) {
    const auto buffers = chain(head); // Validate entire DMA chain before touching disk or guest buffers.
    size_t readable = 0, writable = 0;
    for (const auto &b : buffers) (b.writable ? writable : readable) += b.length;
    if (readable < 16 || writable < 1) throw std::runtime_error("Short virtio block header/status");
    std::array<uint8_t, 16> header{};
    copy(buffers, false, 0, header, false);
    uint32_t type = uint32_t(little(header.data(), 4));
    uint64_t sector = little(header.data() + 8, 8);
    uint8_t result = 2; // UNSUPP
    uint32_t usedLength = 1;
    if (type == 0 || type == 1) {
        size_t length = type == 0 ? writable - 1 : readable - 16;
        if (length > maxRequest || length % 512 || sector > disk.sectors || length / 512 > disk.sectors - sector)
            result = 1; // IOERR
        else {
            std::vector<uint8_t> data(std::min(length, transferChunk));
            bool success = true;
            for (size_t offset = 0; offset < length && success; offset += transferChunk) {
                auto part = std::span<uint8_t>(data).first(std::min(transferChunk, length - offset));
                if (type == 0) {
                    success = disk.read(sector + offset / 512, unsigned(part.size() / 512), part.data());
                    if (success) copy(buffers, true, offset, part, true);
                } else {
                    copy(buffers, false, 16 + offset, part, false);
                    success = disk.write(sector + offset / 512, unsigned(part.size() / 512), part.data());
                }
            }
            if (success) {
                if (type == 0) { bytesRead += length; usedLength += length; }
                else bytesWritten += length;
            }
            result = success ? 0 : 1;
        }
    } else if (type == 4) {
        if (!(driverFeatures & (1ull << 9))) result = 2;
        else { disk.flush(); result = 0; }
    } else if (type == 8) {
        if (writable - 1 < 20) result = 1;
        else {
            std::array<uint8_t, 20> id{'P','C','E','M','U','-','V','I','R','T','I','O','-','B','L','K','0','0','0','1'};
            copy(buffers, true, 0, id, true); result = 0; usedLength += 20;
        }
    }
    copy(buffers, true, writable - 1, std::span<uint8_t>(&result, 1), true);
    ++requests;
    return usedLength;
}
void VirtioBlock::notify() {
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
