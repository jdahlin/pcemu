#include "mmio_space.hpp"
#include "pci_ecam.hpp"
#include "arm_ramfb.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
using namespace pc;
static unsigned checks = 0;
static void check(bool condition) {
    ++checks;
    if (!condition) throw std::runtime_error("Device bus check " + std::to_string(checks));
}
template<class F> static void rejects(F operation) {
    bool rejected = false;
    try { operation(); } catch (const std::invalid_argument &) { rejected = true; }
    check(rejected);
}
int main() {
    MmioSpace space;
    unsigned calls = 0;
    uint64_t seenOffset = 0;
    auto handler = [&](MmioAccess &a, uint64_t offset) {
        ++calls; seenOffset = offset;
        if (!a.write) a.value = UINT64_MAX;
    };
    space.map("registers", 0x1000, 0x10, handler, handler, 1 | 2 | 4);
    MmioAccess a{0x1000, 0, 4, false};
    check(space.access(a) && a.value == UINT64_MAX && seenOffset == 0 && calls == 1);
    a = {0x100f, 7, 4, true};
    check(space.access(a) && seenOffset == 15 && calls == 2); // Device validates the end.
    a.address = 0x1010;
    check(!space.access(a));
    a = {0x1000, 0, 8, false};
    check(!space.access(a));
    a.width = 3;
    rejects([&] { space.access(a); });
    rejects([&] { space.map("overlap", 0xfff, 2, handler, {}); });
    rejects([&] { space.map("empty", 0, 0, handler, {}); });
    rejects([&] { space.map("overflow", UINT64_MAX, 2, handler, {}); });
    space.map("write-only", 0x2000, 4, {}, handler, 4);
    a = {0x2000, 0, 4, false}; check(!space.access(a));
    a.write = true; check(space.access(a));
    space.mapDynamic("fallback", [](const MmioAccess &) { return true; },
        [](MmioAccess &a, uint64_t) { a.value = 7; });
    a = {0x1000, 0, 4, false}; check(space.access(a) && a.value == UINT64_MAX);
    a.address = 0x3000; check(space.access(a) && a.value == 7);
    space.clear(); check(!space.access(a));

    PciEcamBus bus;
    uint64_t bar = 0x10000000, seen = 0;
    bool enabled = true;
    auto attach = [&](unsigned slot, uint64_t identity) {
        bus.attach("test", slot,
            [identity](uint64_t offset, unsigned) { return offset ? offset : identity; },
            [&](uint64_t offset, uint64_t value, unsigned) { seen = offset; bar = value; },
            [&](uint64_t address, unsigned width) {
                return enabled && address >= bar && address - bar < 16 && width <= 16 - (address - bar);
            }, [identity](uint64_t, unsigned) { return identity; },
            [&](uint64_t, uint64_t value, unsigned) { seen = value; });
    };
    attach(2, 0x1234);
    check(bus.configRead(2 * 0x8000, 4) == 0x1234);
    check(bus.configRead(2 * 0x8000 + 0x1000, 4) == UINT64_MAX); // Function one.
    check(bus.configRead(0, 4) == UINT64_MAX);
    check(bus.configRead(0x100000, 4) == UINT64_MAX);
    check(bus.read(bar, 4) == 0x1234 && !bus.read(bar + 16, 1));
    check(bus.write(bar, 55, 4) && seen == 55);
    bus.configWrite(2 * 0x8000 + 0x10, 0x20000000, 4);
    check(seen == 0x10 && !bus.contains(0x10000000, 4) && bus.contains(bar, 4));
    enabled = false; check(!bus.contains(bar, 4)); enabled = true;
    attach(3, 0x5678);
    check(bus.read(bar, 4) == 0x5678); // Last registered BAR wins.
    rejects([&] { attach(2, 1); }); rejects([&] { attach(32, 1); });
    bus.clear(); check(!bus.contains(bar, 1) && bus.configRead(0, 4) == UINT64_MAX);

    std::vector<uint8_t> ram(64), descriptor(28);
    auto field = [&](size_t offset, unsigned width, uint64_t value) {
        for (unsigned i = 0; i < width; ++i) descriptor[offset + width - 1 - i] = value >> (8 * i);
    };
    auto mode = [&] {
        ArmFwCfg config(ram, 0x40000000);
        config.addFile("etc/ramfb", descriptor, true);
        return ArmRamFb(config, ram, 0x40000000).mode();
    };
    ArmFwCfg absent(ram, 0x40000000);
    check(ArmRamFb(absent, ram, 0x40000000).mode() == "invalid");
    absent.addFile("etc/ramfb", std::vector<uint8_t>(4));
    check(ArmRamFb(absent, ram, 0x40000000).mode() == "invalid");
    check(mode() == "inactive");
    field(0, 8, 0x40000008); field(8, 4, 0x34325258);
    field(16, 4, 2); field(20, 4, 2); field(24, 4, 12);
    check(mode() == "2x2x32");
    field(24, 4, 4); check(mode() == "invalid"); field(24, 4, 12);
    field(0, 8, 0x40000038); check(mode() == "invalid"); field(0, 8, 0x40000008);
    ArmFwCfg config(ram, 0x40000000);
    config.addFile("etc/ramfb", descriptor, true);
    ArmRamFb fb(config, ram, 0x40000000);
    ram[8] = 3; ram[9] = 2; ram[10] = 1;
    ram[20] = 6; ram[21] = 5; ram[22] = 4;
    auto output = std::filesystem::temp_directory_path() / ("pcemu-ramfb-" + std::to_string(getpid()) + ".ppm");
    fb.save(output.string());
    std::ifstream input(output, std::ios::binary);
    std::string pixels((std::istreambuf_iterator<char>(input)), {});
    input.close(); std::filesystem::remove(output);
    check(pixels == std::string("P6\n2 2\n255\n") + std::string("\1\2\3\0\0\0\4\5\6\0\0\0", 12));
    std::cout << checks << " MMIO/PCI/framebuffer checks passed\n";
}
