#include "intel_hda.hpp"
#include <cassert>
#include <iostream>
#include <vector>
using namespace pc;
struct Guest {
    static constexpr uint64_t base = 0x40000000, bar = 0x10000000;
    std::vector<uint8_t> ram = std::vector<uint8_t>(65536);
    std::vector<int16_t> played;
    bool irq = false;
    IntelHda hda{ram, base, [this](bool level) { irq = level; },
                 [this](std::span<const int16_t> samples) {
                     played.insert(played.end(), samples.begin(), samples.end());
                 }};
    void put(unsigned offset, uint64_t value, unsigned width = 4) {
        for (unsigned i = 0; i < width; ++i)
            ram.at(offset + i) = uint8_t(value >> (8 * i));
    }
    uint64_t get(unsigned offset, unsigned width = 4) const {
        uint64_t value = 0;
        for (unsigned i = 0; i < width; ++i)
            value |= uint64_t(ram.at(offset + i)) << (8 * i);
        return value;
    }
    void w(unsigned offset, uint64_t value, unsigned width = 4) {
        hda.write_register(bar + offset, value, width);
    }
    uint64_t r(unsigned offset, unsigned width = 4) const {
        return hda.read_register(bar + offset, width);
    }
    uint32_t verb(unsigned node, unsigned opcode, unsigned payload = 0) {
        w(0x60, (node << 20) | (opcode << 8) | payload);
        w(0x68, 3, 2);
        assert(r(0x68, 2) == 2);
        return uint32_t(r(0x64));
    }
    Guest() {
        hda.write_configuration(0x10, bar, 4);
        hda.write_configuration(4, 6, 2);
        w(8, 1);
    }
    void rings(unsigned size = 2) {
        w(0x40, base + 0x1000, 8);
        w(0x50, base + 0x2000, 8);
        w(0x4e, size, 1);
        w(0x5e, size, 1);
        w(0x4a, 0x8000, 2);
        assert(r(0x4a, 2) == 0x8000);
        w(0x4a, 0, 2);
        assert(r(0x4a, 2) == 0);
        w(0x58, 0x8000, 2);
        assert(r(0x58, 2) == 0);
        w(0x5a, 1, 2);
        w(0x5c, 3, 1);
        w(0x4c, 3, 1);
        w(0x20, 0xc0000001);
    }
    void playback() {
        assert(verb(2, 0x706, 0x10) == 0);
        assert(verb(2, 0x200, 0x11) == 0);
        put(0x3000, base + 0x4000, 8);
        put(0x3008, 192);
        put(0x300c, 1);
        put(0x3010, base + 0x4100, 8);
        put(0x3018, 192);
        put(0x301c, 1);
        for (unsigned i = 0; i < 96; ++i) {
            put(0x4000 + i * 2, i + 1, 2);
            put(0x4100 + i * 2, 1000 + i, 2);
        }
        w(0x98, base + 0x3000, 8);
        w(0x88, 384);
        w(0x8c, 1, 2);
        w(0x92, 0x11, 2);
        w(0x70, (base + 0x5000) | 1, 8);
        w(0x20, 0x80000001);
        w(0x80, (1 << 20) | 0x1e, 3);
    }
};
int main() {
    {
        Guest g;
        assert(g.hda.read_configuration(0, 4) == 0x293e8086);
        assert(g.hda.read_configuration(8, 4) == 0x04030003);
        assert(g.hda.read_configuration(0x3d, 1) == 1);
        assert(g.hda.read_configuration(0, 0) == ~uint64_t(0));
        g.hda.write_configuration(0x10, 0xffffffff, 4);
        g.hda.write_configuration(0x14, 0xffffffff, 4);
        assert(g.hda.read_configuration(0x10, 4) == 0xffffc004);
        assert(g.hda.read_configuration(0x14, 4) == 0xffffffff);
        g.hda.write_configuration(0x10, Guest::bar, 4);
        g.hda.write_configuration(0x14, 0, 4);
        assert(g.hda.contains(Guest::bar + 0x3fff, 1));
        assert(!g.hda.contains(Guest::bar + 0x3fff, 2));
        g.hda.write_configuration(4, 4, 1);
        assert(!g.hda.contains(Guest::bar, 4));
        g.hda.write_configuration(4, 6, 1);
        assert(g.r(0, 2) == 0x1001);
        assert(g.r(2, 2) == 0x100);
        assert(g.r(0xe, 2) == 1);
        g.w(0xe, 1, 2);
        assert(!g.r(0xe, 2));
        g.hda.service_elapsed_microseconds(1000);
        assert(g.r(0x30) == 24000);
        assert(g.r(0x2030) == 24000);
        assert(g.verb(0, 0xf00, 0) == 0x1af40011);
        assert(g.verb(0, 0xf00, 4) == 0x10001);
        assert(g.verb(1, 0xf00, 4) == 0x20002);
        assert(g.verb(1, 0xf00, 5) == 1);
        assert(g.verb(2, 0xf00, 9) == 0x401);
        assert(g.verb(3, 0xf00, 9) == 0x400501);
        assert(g.verb(1, 0xf00, 0xa) == 0x20040);
        assert(g.verb(3, 0xf02) == 2);
        assert(g.verb(3, 0xf1c) == 0x90170110);
        g.verb(2, 0x705, 3);
        assert(g.verb(2, 0xf05) == 0x33);
        g.verb(2, 0x706, 0x21);
        assert(g.verb(2, 0xf06) == 0x21);
        g.verb(2, 0x200, 0x4011);
        assert(g.verb(2, 0xa00) == 0x4011);
        g.verb(1, 0x7ff);
        assert(g.verb(2, 0xf06) == 0);
        assert(g.verb(2, 0xa00) == 0x11);
        g.w(0x68, 2, 2);
        assert(g.r(0x68, 2) == 0);
        g.w(8, 0);
        assert(g.r(8) == 0);
        assert(g.r(0x30) == 0);
        assert(g.hda.read_configuration(4, 2) == 6);
    }
    for (unsigned size : {0u, 1u, 2u}) {
        Guest g;
        g.rings(size);
        unsigned entries = size == 0 ? 2 : size == 1 ? 16 : 256;
        for (unsigned i = 1; i < entries + 3; ++i) {
            unsigned slot = i % entries;
            g.put(0x1000 + slot * 4, 0x000f0000);
            g.w(0x48, slot, 2);
            assert(g.r(0x4a, 2) == slot);
            assert(g.r(0x58, 2) == slot);
            assert(g.get(0x2000 + slot * 8, 8) == 0x1af40011);
            assert(g.irq);
            assert(g.r(0x24) == 0xc0000000);
            g.hda.write_configuration(5, 4, 1);
            assert(!g.irq);
            g.hda.write_configuration(5, 0, 1);
            assert(g.irq);
            g.w(0x5d, 1, 1);
            assert(!g.irq);
        }
        g.w(0x5a, 4, 2);
        g.put(0x1000 + (3 % entries) * 4, 0x000f0000);
        g.w(0x48, 3 % entries, 2);
        assert(!g.irq);
        g.hda.service_elapsed_microseconds(1000);
        assert(g.irq);
    }
    {
        Guest g;
        g.rings();
        g.hda.write_configuration(4, 2, 1);
        g.put(0x1004, 0x000f0000);
        g.w(0x48, 1, 2);
        assert(!g.r(0x4a, 2));
        g.hda.write_configuration(4, 6, 1);
        assert(g.r(0x4a, 2) == 1);
        g.w(0x40, 0xfffffffffffff000ull, 8);
        g.w(0x48, 2, 2);
        assert(g.r(0x4d, 1) == 1);
        assert(g.irq);
        g.w(0x4c, 0, 1);
        g.w(0x4d, 1, 1);
        assert(!g.r(0x4d, 1));
    }
    {
        Guest g;
        g.playback();
        g.hda.service_elapsed_microseconds(500);
        assert(g.r(0x84) == 96);
        assert(g.r(0x2084) == 96);
        assert(g.get(0x5000, 8) == 96);
        assert(!g.irq);
        assert(g.played.size() == 48 && g.played.front() == 1 && g.played.back() == 48);
        g.hda.service_elapsed_microseconds(500);
        assert(g.r(0x84) == 192 && g.irq);
        assert(g.r(0x24) == 0x80000001);
        g.w(0x83, 4, 1);
        assert(!g.irq);
        g.hda.service_elapsed_microseconds(1000);
        assert(g.r(0x84) == 0 && g.irq);
        assert(g.played.size() == 192);
        assert(g.played[96] == 1000 && g.played.back() == 1095);
        g.w(0x38, 1);
        g.hda.service_elapsed_microseconds(1000);
        assert(g.r(0x84) == 0);
        g.w(0x38, 0);
        g.hda.service_elapsed_microseconds(1);
        assert(g.r(0x84) == 0);
        g.hda.service_elapsed_microseconds(20);
        assert(g.r(0x84) == 4);
        g.w(0x80, 1, 1);
        assert(g.r(0x84) == 0 && !g.irq && g.r(0x80, 1) == 1);
        g.w(0x80, 0, 1);
        assert(g.r(0x80, 1) == 0);
    }
    {
        Guest g;
        g.playback();
        g.verb(3, 0x707, 0);
        g.hda.service_elapsed_microseconds(1000);
        assert(g.played.empty() && g.r(0x84) == 192);
    }
    {
        Guest g;
        g.rings();
        g.w(0x50, 0xfffffffffffff000ull, 8);
        g.put(0x1004, 0x000f0000);
        g.w(0x48, 1, 2);
        assert(g.r(0x5d, 1) == 4 && g.r(0x4a, 2) == 0);
        g.w(0x5c, 7, 1);
        assert(g.irq);
        g.w(0x50, Guest::base + 0x2000, 8);
        assert(g.r(0x4a, 2) == 1);
    }
    {
        Guest g;
        g.rings();
        g.put(0x1004, 0x100f0000); // No codec at address 1: no reply.
        g.w(0x48, 1, 2);
        assert(g.r(0x4a, 2) == 1 && g.r(0x58, 2) == 0 && !g.irq);
    }
    {
        Guest g;
        g.playback();
        g.hda.write_configuration(4, 2, 1); // Bus mastering disabled.
        g.hda.service_elapsed_microseconds(1000);
        assert(g.r(0x84) == 0 && g.played.empty());
        g.hda.write_configuration(4, 6, 1);
        g.verb(1, 0x705, 3); // Function group in D3: DMA runs, speaker is silent.
        g.hda.service_elapsed_microseconds(1000);
        assert(g.r(0x84) == 192 && g.played.empty());
        g.verb(1, 0x705, 0);
        g.hda.service_elapsed_microseconds(1000);
        assert(g.played.size() == 96);
        assert(g.hda.diagnostics().find("hdaOutputFrames=48") != std::string::npos);
    }
    for (unsigned fault = 0; fault < 6; ++fault) {
        Guest g;
        g.playback();
        if (fault == 0)
            g.w(0x98, 0xfffffffffffff000ull, 8);
        if (fault == 1)
            g.put(0x3008, 0);
        if (fault == 2)
            g.put(0x3000, Guest::base + g.ram.size(), 8);
        if (fault == 3)
            g.w(0x92, 0x4011, 2);
        if (fault == 4)
            g.w(0x88, 0);
        if (fault == 5)
            g.w(0x8c, 0, 2);
        g.hda.service_elapsed_microseconds(1000);
        assert(g.r(0x83, 1) & 16);
        assert(!(g.r(0x80, 1) & 2));
        assert(g.irq);
        assert(g.played.empty());
    }
    std::cout << "Intel HDA tests passed\n";
}
