#include "virtio_blk.hpp"
#include "virtio_pci.hpp"
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
using namespace pc;
namespace fs = std::filesystem;
static unsigned checks = 0;
static void check(bool value) {
    ++checks;
    if (!value) throw std::runtime_error("Virtio check " + std::to_string(checks));
}
struct Fixture {
    static constexpr uint64_t base = 0x40000000;
    std::vector<uint8_t> ram = std::vector<uint8_t>(1024 * 1024);
    bool irq = false;
    std::function<void(bool)> irqObserver;
    Disk disk;
    VirtioBlock device;
    uint16_t next = 0;
    Fixture(const fs::path &path) : disk(path.string()), device(disk, ram, base, [&](bool v) { irq = v; if (irqObserver) irqObserver(v); }) {}
    void put(unsigned offset, uint64_t value, unsigned width) {
        for (unsigned n = 0; n < width; ++n) ram.at(offset + n) = value >> (n * 8);
    }
    uint64_t get(unsigned offset, unsigned width) {
        uint64_t value = 0;
        for (unsigned n = 0; n < width; ++n) value |= uint64_t(ram.at(offset + n)) << (n * 8);
        return value;
    }
    void descriptor(unsigned index, uint64_t address, unsigned length, unsigned flags, unsigned link = 0) {
        auto o = 0x1000 + index * 16;
        put(o, address, 8); put(o + 8, length, 4); put(o + 12, flags, 2); put(o + 14, link, 2);
    }
    void configure() {
        device.write(0x70, 0); next = 0; std::fill(ram.begin(), ram.end(), 0);
        device.write(0x70, 1); device.write(0x70, 3);
        device.write(0x24, 1); device.write(0x20, 1);
        device.write(0x24, 0); device.write(0x20, (1 << 6) | (1 << 9));
        device.write(0x70, 11);
        check(device.read(0x70) == 11);
        device.write(0x30, 0); device.write(0x38, 8);
        device.write(0x80, base + 0x1000); device.write(0x90, base + 0x2000); device.write(0xa0, base + 0x3000);
        device.write(0x44, 1); device.write(0x70, 15);
    }
    void request(unsigned type, uint64_t lba, unsigned length = 512) {
        put(0x4000, type, 4); put(0x4008, lba, 8);
        ram[0x7000] = 0xa5;
        descriptor(0, base + 0x4000, 16, 1, 1);
        descriptor(1, base + 0x5000, length, type == 1 ? 1 : 3, 2);
        descriptor(2, base + 0x7000, 1, 2);
    }
    void notify() {
        put(0x2004 + (next % 8) * 2, 0, 2); put(0x2002, ++next, 2);
        device.write(0x50, 0);
    }
};
int main() {
    auto directory = fs::temp_directory_path() / ("pcemu-virtio-" + std::to_string(getpid()));
    fs::create_directory(directory);
    try {
        auto parent = directory / "base.raw", child = directory / "delta.pcd";
        { std::ofstream out(parent, std::ios::binary); std::array<char, 8192> zeros{}; out.write(zeros.data(), zeros.size()); }
        Disk::createOverlay(child.string(), parent.string());
        {
            Fixture f(child);
            check(f.device.read(0) == 0x74726976 && f.device.read(4) == 2 && f.device.read(8) == 2);
            check(f.device.read(0x100, 8) == 16 && f.device.read(0x114) == 512);
            f.device.write(0x14, 1); check(f.device.read(0x10) == 1);
            f.device.write(0x30, 99); check(f.device.read(0x34) == 0);
            f.configure(); f.request(1, 2);
            for (unsigned i = 0; i < 512; ++i) f.ram[0x5000 + i] = uint8_t(i ^ 0x5a);
            f.notify();
            check(f.ram[0x7000] == 0 && f.get(0x3002, 2) == 1 && f.get(0x3008, 4) == 1);
            check(f.irq && f.device.read(0x60) == 1);
            f.device.write(0x64, 1); check(!f.irq);
            f.request(0, 2); std::fill_n(f.ram.begin() + 0x5000, 512, 0);
            f.notify();
            check(f.ram[0x7000] == 0 && f.get(0x3010, 4) == 513);
            for (unsigned i = 0; i < 512; ++i) check(f.ram[0x5000 + i] == uint8_t(i ^ 0x5a));
            check(f.device.bytesRead == 512 && f.device.bytesWritten == 512);
            f.request(4, 0, 0); f.notify(); check(f.ram[0x7000] == 0);
            f.request(0, UINT64_MAX); f.notify(); check(f.ram[0x7000] == 1 && f.device.error().empty());
            f.request(0xbeef, 0, 0); f.notify(); check(f.ram[0x7000] == 2);
            f.disk.flush();
            Disk original(parent.string(), false, true);
            std::array<uint8_t, 512> bytes{}; check(original.read(2, 1, bytes.data()));
            check(std::all_of(bytes.begin(), bytes.end(), [](uint8_t b) { return b == 0; }));

            f.configure(); f.request(8, 0, 20); f.notify();
            check(std::string(reinterpret_cast<char *>(f.ram.data() + 0x5000), 20) == "PCEMU-VIRTIO-BLK0001");
            for (unsigned i = 0; i < 65536; ++i) f.notify();
            check(f.device.error().empty() && f.get(0x3002, 2) == 1); // uint16 wrap, ring reuse

            // Header split over descriptors, payload and status in one buffer.
            f.configure(); f.request(0, 2);
            f.descriptor(0, Fixture::base + 0x4000, 8, 1, 1);
            f.descriptor(1, Fixture::base + 0x4008, 8, 1, 2);
            f.descriptor(2, Fixture::base + 0x5000, 513, 2);
            f.notify(); check(f.ram[0x5200] == 0 && f.ram[0x5000] == 0x5a);

            // Never follow guest-provided DMA outside RAM, or loop on a cycle.
            f.configure(); f.request(0, 0); f.descriptor(1, UINT64_MAX - 511, 512, 3, 2); f.notify();
            check((f.device.read(0x70) & 64) && f.get(0x3002, 2) == 0 && f.ram[0x7000] == 0xa5);
            check(f.device.read(0x60) == 2 && f.irq);
            f.configure(); f.request(0, 0); f.descriptor(0, Fixture::base + 0x4000, 16, 1, 0); f.notify();
            check((f.device.read(0x70) & 64) && f.get(0x3002, 2) == 0);
            f.configure(); f.request(0, 0); f.descriptor(0, Fixture::base + 0x4000, 16, 5, 1); f.notify();
            check(f.device.read(0x70) & 64); // Indirect not offered.
            f.configure(); f.put(0x2002, 9, 2); f.device.write(0x50, 0);
            check(f.device.read(0x70) & 64); // Available ring overrun.
            f.configure(); f.device.write(0x80, 0); check(f.device.read(0x70) & 64);
            f.configure(); f.device.write(0x70, 0); f.device.write(0x38, 257); f.device.write(0x44, 1);
            check(f.device.read(0x70) & 64);
            f.configure(); f.device.write(0x70, 0); f.device.write(0x38, 8); f.device.write(0x84, 0xffffffff); f.device.write(0x44, 1);
            check(f.device.read(0x70) & 64);
            f.device.write(0x70, 0); f.device.write(0x24, 1); f.device.write(0x20, 2); f.device.write(0x70, 11);
            check(!(f.device.read(0x70) & 8)); // Unknown/missing VERSION_1 rejected.
            // Oversized but in-RAM scatter buffers return IOERR without a
            // huge host allocation, corrupting data, or requiring device reset.
            f.configure(); f.device.write(0x44, 0); f.device.write(0x38, 64); f.device.write(0x44, 1);
            f.request(0, 0);
            for (unsigned i = 1; i <= 33; ++i)
                f.descriptor(i, Fixture::base + 0x5000, 512 * 1024, 3, i + 1);
            f.descriptor(34, Fixture::base + 0x7000, 1, 2);
            f.ram[0x5000] = 0x5a; f.notify();
            check(f.ram[0x7000] == 1 && f.ram[0x5000] == 0x5a && !(f.device.read(0x70) & 64));
            f.device.write(0x70, 0);
            bool pciLine = false;
            VirtioPciBlock pci(f.device, [&](bool level) { pciLine = level; });
            f.irqObserver = [&](bool level) { pci.irq(level); };
            check(pci.configRead(0, 4) == 0x10421af4 && pci.configRead(0x2e, 2) >= 0x40);
            check(pci.configRead(0x1000, 4) == 0xffffffff);
            unsigned cap = pci.configRead(0x34, 1), caps = 0;
            while (cap) {
                check(pci.configRead(cap, 1) == 9);
                caps |= 1 << (pci.configRead(cap + 3, 1) - 1);
                cap = pci.configRead(cap + 1, 1);
            }
            check(caps == 15);
            pci.configWrite(0x10, 0xffffffff, 4); check(pci.configRead(0x10, 4) == 0xffffc000);
            pci.configWrite(0x10, 0x10000000, 4); pci.configWrite(4, 6, 2);
            constexpr uint64_t bar = 0x10000000;
            check(pci.contains(bar, 4) && !pci.contains(bar - 1, 4));
            pci.write(bar + 20, 0, 1); check(pci.read(bar + 24, 2) == 256);
            pci.write(bar + 20, 1, 1); pci.write(bar + 20, 3, 1);
            pci.write(bar + 8, 1, 4); pci.write(bar + 12, 1, 4);
            pci.write(bar + 8, 0, 4); pci.write(bar + 12, (1 << 6) | (1 << 9), 4);
            pci.write(bar + 20, 11, 1); pci.write(bar + 22, 0, 2); pci.write(bar + 24, 8, 2);
            pci.write(bar + 32, Fixture::base + 0x1000, 8);
            pci.write(bar + 40, Fixture::base + 0x2000, 8);
            pci.write(bar + 48, Fixture::base + 0x3000, 8);
            pci.write(bar + 28, 1, 2); pci.write(bar + 20, 15, 1);
            std::fill(f.ram.begin(), f.ram.end(), 0); f.next = 0;
            f.request(0, 2); f.put(0x2004, 0, 2); f.put(0x2002, 1, 2);
            pci.write(bar + 0x1000, 0, 2);
            check(f.ram[0x7000] == 0 && f.ram[0x5000] == 0x5a && pciLine);
            pci.configWrite(4, 0x406, 2); check(!pciLine);
            pci.configWrite(4, 6, 2); check(pciLine);
            check(pci.read(bar + 0x2000, 1) == 1 && !pciLine);
            check(pci.read(bar + 0x3000, 8) == 16);
            f.irqObserver = {};
        }
        fs::remove_all(directory);
        std::cout << checks << " virtio checks passed\n";
    } catch (...) { fs::remove_all(directory); throw; }
}
