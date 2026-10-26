#pragma once
#include "virtio_device.hpp"
#include <array>
#include <stdexcept>
#include <vector>
namespace tooltest {
inline unsigned checks = 0;
inline void check(bool value) {
    ++checks;
    if (!value)
        throw std::runtime_error("guest-tool check " + std::to_string(checks));
}
struct Memory {
    static constexpr uint64_t base = 0x40000000;
    std::vector<uint8_t> ram = std::vector<uint8_t>(2 * 1024 * 1024);
    std::array<uint16_t, 8> submitted{};
    void put(size_t at, uint64_t v, unsigned width) {
        for (unsigned i = 0; i < width; ++i)
            ram.at(at + i) = uint8_t(v >> (8 * i));
    }
    uint64_t get(size_t at, unsigned width) const {
        uint64_t value = 0;
        for (unsigned i = 0; i < width; ++i)
            value |= uint64_t(ram.at(at + i)) << (8 * i);
        return value;
    }
    static size_t desc(unsigned q) {
        return 0x1000 + q * 0x3000;
    }
    static size_t avail(unsigned q) {
        return desc(q) + 0x1000;
    }
    static size_t used(unsigned q) {
        return desc(q) + 0x2000;
    }
    void configure(pc::VirtioDevice &dev, unsigned queues, uint32_t features) {
        dev.write(0x70, 0);
        std::fill(ram.begin(), ram.end(), 0);
        submitted.fill(0);
        dev.write(0x70, 3);
        dev.write(0x24, 0);
        dev.write(0x20, features);
        dev.write(0x24, 1);
        dev.write(0x20, 1);
        dev.write(0x70, 11);
        for (unsigned q = 0; q < queues; ++q) {
            dev.write(0x30, q);
            dev.write(0x38, 8);
            dev.write(0x80, base + desc(q));
            dev.write(0x90, base + avail(q));
            dev.write(0xa0, base + used(q));
            dev.write(0x44, 1);
        }
        dev.write(0x70, 15);
        check(dev.error().empty());
    }
    void descriptor(unsigned q, unsigned index, size_t at, size_t size, unsigned flags,
                    unsigned next = 0) {
        auto off = desc(q) + index * 16;
        put(off, base + at, 8);
        put(off + 8, size, 4);
        put(off + 12, flags, 2);
        put(off + 14, next, 2);
    }
    void submit(pc::VirtioDevice &dev, unsigned q, unsigned head = 0) {
        put(avail(q) + 4 + (submitted[q] % 8) * 2, head, 2);
        put(avail(q) + 2, ++submitted[q], 2);
        dev.write(0x50, q);
    }
};
} // namespace tooltest
