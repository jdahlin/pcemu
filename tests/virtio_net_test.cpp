#include "virtio_net.hpp"
#include "virtio_pci.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>
using namespace pc;
static unsigned checks;
void check(bool condition) {
    ++checks;
    if (!condition)
        throw std::runtime_error("virtio-net check " + std::to_string(checks));
}
struct Fixture {
    static constexpr uint64_t base = 0x40000000;
    std::vector<uint8_t> ram = std::vector<uint8_t>(65536);
    bool irq = false;
    VirtioNet net{ram, base, [&](bool level) { irq = level; }};
    void put(unsigned at, uint64_t value, unsigned width) {
        for (unsigned i = 0; i < width; ++i)
            ram.at(at + i) = value >> (8 * i);
    }
    uint64_t get(unsigned at, unsigned width) {
        uint64_t v = 0;
        for (unsigned i = 0; i < width; ++i)
            v |= uint64_t(ram.at(at + i)) << (8 * i);
        return v;
    }
    void configure(bool event = false) {
        net.write(0x70, 0);
        std::fill(ram.begin(), ram.end(), 0);
        net.write(0x70, 3);
        net.write(0x24, 1);
        net.write(0x20, 1);
        net.write(0x24, 0);
        net.write(0x20, 1 | (event ? 1u << 29 : 0));
        net.write(0x70, 11);
        for (unsigned q = 0; q < 2; ++q) {
            net.write(0x30, q);
            net.write(0x38, 8);
            net.write(0x80, base + 0x1000 + q * 0x3000);
            net.write(0x90, base + 0x2000 + q * 0x3000);
            net.write(0xa0, base + 0x3000 + q * 0x3000);
            net.write(0x44, 1);
        }
        net.write(0x70, 15);
        check(net.error().empty());
    }
    void descriptor(unsigned queue, unsigned index, unsigned address, unsigned length,
                    unsigned flags, unsigned next = 0) {
        unsigned at = 0x1000 + queue * 0x3000 + index * 16;
        put(at, base + address, 8);
        put(at + 8, length, 4);
        put(at + 12, flags, 2);
        put(at + 14, next, 2);
    }
    void tx(unsigned length = 72) {
        descriptor(1, 0, 0x8000, length, 0);
        put(0x5004, 0, 2);
        put(0x5002, 1, 2);
        net.write(0x50, 1);
    }
};
int main() {
    Fixture f;
    check(f.net.read(8) == 1 && f.net.read(4) == 2);
    check(f.net.read(0x100, 2) == 0x5452 && f.net.read(0x106, 2) == 1 &&
          f.net.read(0x10a, 2) == 1500);
    f.net.write(0x30, 2);
    check(f.net.read(0x34) == 0);
    f.configure();
    for (unsigned i = 12; i < 72; ++i)
        f.ram[0x8000 + i] = i;
    f.tx();
    check(f.net.transmitted == 1 && f.net.outgoing.size() == 1 && f.irq);
    check(f.net.outgoing.front().size() == 60 && f.net.outgoing.front().front() == 12);
    check(f.get(0x6002, 2) == 1 && f.get(0x6008, 4) == 0);
    f.net.write(0x64, 1);
    check(!f.irq);
    f.descriptor(0, 0, 0x9000, 16, 3, 1);
    f.descriptor(0, 1, 0xa000, 2048, 2);
    f.put(0x2004, 0, 2);
    f.put(0x2002, 1, 2);
    std::vector<uint8_t> frame(60, 0xa5);
    f.net.receive(frame.data(), frame.size());
    check(f.net.received == 1 && f.get(0x3008, 4) == 72 && f.irq);
    check(
        std::all_of(f.ram.begin() + 0x9000, f.ram.begin() + 0x900c, [](auto b) { return b == 0; }));
    check(f.ram[0x900c] == 0xa5 && f.ram[0xa000] == 0xa5);
    f.net.receive(frame.data(), frame.size());
    check(f.net.dropped == 1);
    f.configure();
    f.ram[0x8000] = 1; // partial checksum over a 60-byte Ethernet packet, seed=0
    f.ram[0x8006] = 14;
    f.ram[0x8008] = 2;
    f.tx();
    check(f.net.error().empty());
    check(f.net.outgoing.front()[16] == 255 && f.net.outgoing.front()[17] == 255);
    f.configure(true);
    f.put(0x5000, 1, 2); // EVENT_IDX supersedes NO_INTERRUPT
    f.put(0x5014, 0, 2);
    f.tx();
    check(f.irq && f.get(0x6044, 2) == 1);
    f.configure(true);
    f.put(0x5014, 1, 2);
    f.tx();
    check(!f.irq);
    f.configure();
    f.descriptor(1, 0, 0x8000, 72, 1, 0);
    f.put(0x5002, 1, 2);
    f.net.write(0x50, 1);
    check(!f.net.error().empty() && (f.net.read(0x70) & 64));
    f.configure();
    f.descriptor(1, 0, 0xfff0, 72, 0);
    f.put(0x5002, 1, 2);
    f.net.write(0x50, 1);
    check(!f.net.error().empty());
    f.configure();
    f.ram[0x8000] = 1;
    f.ram[0x8006] = 255;
    f.tx();
    check(!f.net.error().empty());
    f.configure();
    f.put(0x5002, 9, 2);
    f.net.write(0x50, 1);
    check(!f.net.error().empty());
    f.configure();
    f.descriptor(0, 0, 0x9000, 2048, 0);
    f.put(0x2002, 1, 2);
    f.net.receive(frame.data(), frame.size());
    check(!f.net.error().empty());
    f.net.write(0x70, 0);
    check(f.net.error().empty() && !f.irq && f.net.outgoing.empty());
    f.configure();
    f.put(0x5000, 1, 2);
    f.tx();
    check(!f.irq);
    f.configure();
    for (unsigned i = 0; i < 65538; ++i) {
        f.descriptor(1, 0, 0x8000, 72, 0);
        f.put(0x5004 + (i % 8) * 2, 0, 2);
        f.put(0x5002, uint16_t(i + 1), 2);
        f.net.write(0x50, 1);
        if (!f.net.outgoing.empty())
            f.net.outgoing.pop_front();
    }
    check(f.net.error().empty() && f.get(0x6002, 2) == 2);
    f.configure();
    f.ram[0x9000] = 0xa5;
    f.descriptor(0, 0, 0x9000, 8, 2);
    f.put(0x2002, 1, 2);
    f.net.receive(frame.data(), frame.size());
    check(!f.net.error().empty() && f.ram[0x9000] == 0xa5 && f.get(0x3002, 2) == 0);
    std::cout << checks << " virtio-net checks passed\n";
}
