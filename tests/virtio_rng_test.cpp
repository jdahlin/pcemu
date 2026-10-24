#include "virtio_rng.hpp"
#include "virtio_pci.hpp"
#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
using namespace pc;
static unsigned checks;
static void check(bool condition) {
    ++checks;
    if (!condition) throw std::runtime_error("virtio-rng check " + std::to_string(checks));
}
struct Fixture {
    static constexpr uint64_t base = 0x40000000;
    std::vector<uint8_t> ram = std::vector<uint8_t>(131072, 0);
    bool irq = false;
    unsigned calls = 0;
    VirtioRng rng{ram, base, [&](bool value) { irq = value; },
        [&](std::span<uint8_t> bytes) { ++calls; std::fill(bytes.begin(), bytes.end(), 0xa5); }};
    void put(unsigned offset, uint64_t value, unsigned width) {
        for (unsigned i = 0; i < width; ++i) ram.at(offset + i) = value >> (8 * i);
    }
    uint64_t get(unsigned offset, unsigned width) {
        uint64_t value = 0;
        for (unsigned i = 0; i < width; ++i) value |= uint64_t(ram.at(offset + i)) << (8 * i);
        return value;
    }
    void configure() {
        rng.write(0x70, 0);
        std::fill(ram.begin(), ram.end(), 0);
        calls = 0;
        rng.write(0x70, 3);
        rng.write(0x24, 1); rng.write(0x20, 1);
        rng.write(0x70, 11);
        rng.write(0x38, 8);
        rng.write(0x80, base + 0x1000);
        rng.write(0x90, base + 0x2000);
        rng.write(0xa0, base + 0x3000);
        rng.write(0x44, 1);
        rng.write(0x70, 15);
        check(rng.error().empty());
    }
    void descriptor(unsigned index, uint64_t address, unsigned length, unsigned flags, unsigned next = 0) {
        unsigned offset = 0x1000 + index * 16;
        put(offset, address, 8); put(offset + 8, length, 4);
        put(offset + 12, flags, 2); put(offset + 14, next, 2);
    }
    void submit(unsigned head = 0, unsigned count = 1) {
        put(0x2004, head, 2); put(0x2002, count, 2); rng.write(0x50, 0);
    }
    void broken() {
        check(!rng.error().empty() && (rng.read(0x70) & 64) && irq);
        check(calls == 0 && get(0x3002, 2) == 0);
    }
};
int main() {
    Fixture f;
    check(f.rng.read(8) == 4);
    f.configure();
    f.descriptor(0, f.base + 0x8000, 8, 3, 1);
    f.descriptor(1, f.base + 0x8100, 24, 2);
    f.submit();
    check(f.calls == 2 && f.irq && f.get(0x3002, 2) == 1);
    check(f.get(0x3004, 4) == 0 && f.get(0x3008, 4) == 32);
    check(f.ram[0x8000] == 0xa5 && f.ram[0x8117] == 0xa5 && f.ram[0x8118] == 0);
    f.rng.write(0x64, 1); check(!f.irq);
    f.rng.write(0x50, 0); check(f.calls == 2); // No replay.
    f.configure();
    f.descriptor(0, f.base + 0x8000, 90000, 2);
    f.put(0x2000, 1, 2); // Suppress interrupts, not completion.
    f.submit();
    check(!f.irq && f.get(0x3008, 4) == 65536 && f.ram[0x18000] == 0);
    f.configure(); f.descriptor(0, f.base + 0x8000, 8, 0); f.submit(); f.broken();
    f.configure(); f.descriptor(0, f.base + 0x8000, 8, 3); f.submit(); f.broken();
    f.configure(); f.descriptor(0, f.base - 1, 8, 2); f.submit(); f.broken();
    f.configure(); f.descriptor(0, UINT64_MAX - 3, 8, 2); f.submit(); f.broken();
    f.configure(); f.descriptor(0, f.base + 0x8000, 0, 2); f.submit(); f.broken();
    f.configure(); f.descriptor(0, f.base + 0x8000, 8, 6); f.submit(); f.broken();
    f.configure(); f.submit(8); f.broken();
    f.configure(); f.submit(0, 9); f.broken();
    f.configure();
    f.descriptor(0, f.base + 0x8000, 8, 3, 1);
    f.descriptor(1, f.base - 1, 8, 2);
    f.submit(); f.broken(); check(f.ram[0x8000] == 0); // Validate before filling.
    f.configure(); f.rng.write(0x38, 16); check(!f.rng.error().empty());
    f.rng.write(0x70, 0); check(!f.irq && f.rng.error().empty() && f.rng.read(0x44) == 0);
    f.rng.write(0x24, 1); f.rng.write(0x20, 3); f.rng.write(0x70, 11);
    check(!(f.rng.read(0x70) & 8)); // Reject unadvertised features.
    f.configure();
    VirtioPciDevice pci([&](unsigned o, unsigned w) { return f.rng.read(o, w); },
        [&](unsigned o, uint64_t v, unsigned w) { f.rng.write(o, v, w); },
        4, 0x00ff0001, 1, [](bool) {});
    check(pci.configRead(0, 4) == 0x10441af4);
    // Exercise the real default host entropy source without asserting byte values.
    std::array<uint8_t, 128> first{};
    VirtioRng real(f.ram, f.base, [](bool) {});
    auto fill = [&] {
        real.write(0x70, 0); real.write(0x24, 1); real.write(0x20, 1); real.write(0x70, 11);
        real.write(0x38, 8); real.write(0x80, f.base + 0x1000);
        real.write(0x90, f.base + 0x2000); real.write(0xa0, f.base + 0x3000);
        real.write(0x44, 1); real.write(0x70, 15);
        f.descriptor(0, f.base + 0x8000, 128, 2); f.put(0x2004, 0, 2); f.put(0x2002, 1, 2);
        real.write(0x50, 0); check(real.error().empty());
    };
    fill(); std::copy_n(f.ram.begin() + 0x8000, first.size(), first.begin());
    fill(); check(!std::equal(first.begin(), first.end(), f.ram.begin() + 0x8000));
    std::cout << checks << " virtio-rng checks passed\n";
}
