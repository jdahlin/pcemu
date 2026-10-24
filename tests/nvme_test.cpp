#include "nvme.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
using namespace pc;
static unsigned checks = 0;
void check(bool ok) { ++checks; if (!ok) throw std::runtime_error("NVMe check " + std::to_string(checks)); }
uint64_t get(std::span<const uint8_t> b, size_t o, unsigned n) {
    uint64_t v = 0; for (unsigned i = 0; i < n; ++i) v |= uint64_t(b[o + i]) << (8 * i); return v;
}
void put(std::span<uint8_t> b, size_t o, uint64_t v, unsigned n) {
    for (unsigned i = 0; i < n; ++i) b[o + i] = v >> (8 * i);
}
int main() {
    char path[] = "/tmp/pcemu-nvme-XXXXXX";
    int fd = mkstemp(path); check(fd >= 0); check(ftruncate(fd, 4 * 1024 * 1024) == 0); close(fd);
    struct Cleanup { std::string path; ~Cleanup() { std::filesystem::remove(path); } } cleanup{path};
    Disk disk(path);
    std::vector<uint8_t> ram(1024 * 1024);
    constexpr uint64_t base = 0x40000000, bar = 0x10000000;
    bool irq = false;
    Nvme dev(disk, ram, base, [&](bool level) { irq = level; });
    check(dev.configRead(0, 4) == 0x00101b36);
    check(dev.configRead(8, 4) == 0x01080201);
    check(dev.configRead(4096, 4) == UINT32_MAX);
    dev.configWrite(0x10, UINT32_MAX, 4); check(dev.configRead(0x10, 4) == 0xffffc000);
    dev.configWrite(0x10, bar, 4); check(!dev.contains(bar, 4));
    dev.configWrite(4, 6, 2); check(dev.contains(bar, 4));
    check(dev.read(bar + 8, 4) == 0x10100);
    check((dev.read(bar, 8) & 0xffff) == 255);
    auto write = [&](unsigned offset, uint64_t v, unsigned n = 4) { dev.write(bar + offset, v, n); };
    auto init = [&] {
        write(0x14, 0); write(0x24, 7 | (7 << 16));
        write(0x28, base + 0x1000, 8); write(0x30, base + 0x2000, 8);
        write(0x14, 0x460001); check(dev.read(bar + 0x1c, 4) == 1);
    };
    init();
    unsigned aTail = 0, aHead = 0, cid = 1;
    auto admin = [&](unsigned op, uint32_t ns, uint64_t prp, uint32_t d10, uint32_t d11 = 0) {
        auto cmd = std::span(ram).subspan(0x1000 + aTail * 64, 64); std::fill(cmd.begin(), cmd.end(), 0);
        cmd[0] = op; put(cmd, 2, cid, 2); put(cmd, 4, ns, 4); put(cmd, 24, prp, 8);
        put(cmd, 40, d10, 4); put(cmd, 44, d11, 4);
        aTail = (aTail + 1) % 8; write(0x1000, aTail);
        auto completion = std::span(ram).subspan(0x2000 + aHead * 16, 16);
        check(get(completion, 12, 2) == cid++);
        unsigned status = get(completion, 14, 2) >> 1;
        aHead = (aHead + 1) % 8; write(0x1004, aHead);
        return status;
    };
    check(admin(6, 0, base + 0x5000, 1) == 0);
    check(get(ram, 0x5000, 2) == 0x1b36 && ram[0x5000 + 77] == 5);
    check(ram[0x5000 + 512] == 0x66 && get(ram, 0x5000 + 516, 4) == 1);
    check(admin(6, 1, base + 0x5000, 0) == 0);
    check(get(ram, 0x5000, 8) == disk.sectors && ram[0x5000 + 130] == 9);
    check(admin(6, 0, base + 0x5000, 2) == 0 && get(ram, 0x5000, 4) == 1);
    check(admin(6, 2, base + 0x5000, 0) == 0xb);
    check(admin(6, 0, base + ram.size() - 16, 1) == 4);
    check(admin(0xff, 0, 0, 0) == 1);
    check(admin(9, 0, 0, 7, 0) == 0); // Negotiate one SQ/CQ.
    check(admin(0xa, 0, 0, 7) == 0);
    check(admin(9, 0, 0, 6, 1) == 2); // No volatile cache to enable.
    check(admin(5, 0, base + 0x4000, 1 | (3 << 16), 3) == 0);
    check(admin(1, 0, base + 0x3000, 1 | (3 << 16), 1 | (1 << 16)) == 0);
    check(admin(5, 0, base + 0x4000, 1 | (3 << 16), 3) == 2);
    unsigned tail = 0, head = 0, ioCid = 100;
    bool phase = true;
    auto io = [&](unsigned op, uint64_t lba, unsigned count, uint64_t prp1, uint64_t prp2, unsigned ns = 1) {
        auto cmd = std::span(ram).subspan(0x3000 + tail * 64, 64); std::fill(cmd.begin(), cmd.end(), 0);
        cmd[0] = op; put(cmd, 2, ioCid, 2); put(cmd, 4, ns, 4);
        put(cmd, 24, prp1, 8); put(cmd, 32, prp2, 8); put(cmd, 40, lba, 8); put(cmd, 48, count - 1, 4);
        tail = (tail + 1) % 4; write(0x1008, tail);
        auto completion = std::span(ram).subspan(0x4000 + head * 16, 16);
        check(get(completion, 12, 2) == ioCid++);
        check((get(completion, 14, 2) & 1) == phase);
        unsigned status = get(completion, 14, 2) >> 1;
        check(irq); write(0xc, 1); check(!irq); write(0x10, 1); check(irq);
        dev.configWrite(4, 0x406, 2); check(!irq); dev.configWrite(4, 6, 2); check(irq);
        if (++head == 4) { head = 0; phase = !phase; }
        write(0x100c, head); check(!irq);
        return status;
    };
    for (unsigned i = 0; i < 1024; ++i) ram[0x6ff0 + i] = uint8_t(i * 17);
    check(io(1, 8, 2, base + 0x6ff0, base + 0x7000) == 0);
    std::array<uint8_t, 1024> expected{}; std::copy_n(ram.begin() + 0x6ff0, 1024, expected.begin());
    std::fill(ram.begin() + 0x6ff0, ram.begin() + 0x73f0, 0);
    check(io(2, 8, 2, base + 0x6ff0, base + 0x7000) == 0);
    check(std::equal(expected.begin(), expected.end(), ram.begin() + 0x6ff0));
    // Three-page scatter transfer using a PRP list; all pages are distinct.
    put(ram, 0x9000, base + 0xb000, 8); put(ram, 0x9008, base + 0xc000, 8);
    for (unsigned i = 0; i < 12288; ++i) ram[0xa000 + i] = uint8_t(i * 31 + 9);
    check(io(1, 32, 24, base + 0xa000, base + 0x9000) == 0);
    std::fill(ram.begin() + 0xa000, ram.begin() + 0xd000, 0);
    check(io(2, 32, 24, base + 0xa000, base + 0x9000) == 0);
    for (unsigned i = 0; i < 12288; ++i) check(ram[0xa000 + i] == uint8_t(i * 31 + 9));
    // Invalid scatter tail cannot partially modify the first guest page.
    put(ram, 0x9008, base + ram.size(), 8);
    std::fill(ram.begin() + 0xa000, ram.begin() + 0xb000, 0x55);
    check(io(2, 32, 24, base + 0xa000, base + 0x9000) == 4);
    check(ram[0xa000] == 0x55 && ram[0xafff] == 0x55);
    check(io(1, 32, 24, base + 0xa000, base + 0x9000) == 4);
    check(io(2, disk.sectors, 1, base + 0xa000, 0) == 0x80);
    check(io(2, 0, 257, base + 0xa000, 0) == 2);
    check(io(2, 0, 1, base + 0xa000, 0, 2) == 0xb);
    check(io(0xff, 0, 1, 0, 0) == 1);
    check(io(0, 0, 1, 0, 0) == 0);
    check(admin(4, 0, 0, 1) == 0x10c); // CQ still referenced.
    check(admin(0, 0, 0, 1) == 0);
    check(admin(4, 0, 0, 1) == 0);
    // CQ backpressure: a tiny CQ must resume pending submissions when acknowledged.
    check(admin(5, 0, base + 0x4000, 1 | (1 << 16), 3) == 0);
    check(admin(1, 0, base + 0x3000, 1 | (3 << 16), 1 | (1 << 16)) == 0);
    for (unsigned i = 0; i < 2; ++i) {
        auto cmd = std::span(ram).subspan(0x3000 + i * 64, 64);
        std::fill(cmd.begin(), cmd.end(), 0); // Flush.
        put(cmd, 2, 200 + i, 2); put(cmd, 4, 1, 4);
    }
    write(0x1008, 2);
    check(get(ram, 0x400c, 2) == 200 && irq);
    write(0x100c, 1); // Frees space; second submission completes immediately.
    check(get(ram, 0x401c, 2) == 201 && irq);
    write(0x100c, 0); check(!irq);
    write(0x14, 0); check(!irq && dev.read(bar + 0x1c, 4) == 0);
    write(0x28, base + ram.size(), 8); write(0x14, 0x460001); check(dev.read(bar + 0x1c, 4) == 2);
    init(); write(0x14, 0x464001); check((dev.read(bar + 0x1c, 4) & 12) == 8);
    std::cout << checks << " NVMe checks passed\n";
}
