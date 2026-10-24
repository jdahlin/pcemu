#include "cpu_benchmarks.hpp"
#include "cpu.hpp"
#include "jit.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
namespace pc {
double benchmarkCpuRegisters(bool enabled, uint64_t target) {
    CPU c;
    c.s[CS].big = true;
    unsigned p = 0;
    for (int i = 0; i < 48; i++) {
        c.mem[p++] = 0x83;
        c.mem[p++] = 0xc0;
        c.mem[p++] = 1;
    }
    c.mem[p++] = 0x83;
    c.mem[p++] = 0xf8;
    c.mem[p++] = 0;
    c.mem[p++] = 0xe9;
    int32_t disp = -int32_t(p + 4);
    std::memcpy(&c.mem[p], &disp, 4);
    Jit jit(c, enabled);
    auto begin = std::chrono::steady_clock::now();
    while (c.instructions < target)
        jit.run();
    double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    std::cout << (enabled ? "JIT" : "Interpreter") << ": " << c.instructions / sec / 1e6
              << " million guest instructions/s, " << jit.nativeInstructions << " native, "
              << jit.compiledBlocks << " blocks; EAX=" << c.r[AX] << '\n';
    return sec;
}
double benchmarkCpuMemory(bool enabled, uint64_t target, bool paging, unsigned width) {
    if (width != 1 && width != 4)
        throw std::invalid_argument("Memory benchmark width must be 1 or 4");
    CPU c;
    c.s[CS].big = true;
    unsigned p = 0;
    auto address = [&](uint32_t value) {
        std::memcpy(c.mem.data() + p, &value, 4);
        p += 4;
    };
    for (unsigned cell = 0; cell < 16; cell++) {
        c.mem[p++] = width == 1 ? 0xa0 : 0xa1;
        address(0x1000 + cell * width);
        c.mem[p++] = width == 1 ? 0x80 : 0x83;
        c.mem[p++] = 0xc0;
        c.mem[p++] = 1;
        c.mem[p++] = width == 1 ? 0xa2 : 0xa3;
        address(0x1000 + cell * width);
        if (width == 1)
            c.write(0x1000 + cell, cell + 1, 1);
    }
    c.mem[p++] = 0xe9;
    address(uint32_t(-int32_t(p + 4)));
    if (paging) {
        c.write(0x2000, 0x3007, 4);
        c.write(0x3000, 7, 4);
        c.write(0x3004, 0x1007, 4);
        c.cr[0] = 0x80000011;
        c.cr[3] = 0x2000;
        c.s[CS].selector = 3;
    }
    target = std::max(uint64_t(49), target / 49 * 49);
    Jit jit(c, enabled);
    auto begin = std::chrono::steady_clock::now();
    while (c.instructions < target)
        jit.run();
    double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    uint64_t checksum = 0;
    for (unsigned cell = 0; cell < 16; cell++) {
        auto value = c.read(0x1000 + cell * width, width);
        auto expected = width == 1 ? (cell + 1 + target / 49) & 255u : target / 49;
        if (value != expected)
            throw std::runtime_error("Memory benchmark checksum mismatch");
        checksum += value;
    }
    std::cout << (paging ? "Paged" : "Flat") << (width == 1 ? " byte memory " : " memory ") << (enabled ? "JIT" : "interpreter")
              << ": " << c.instructions / seconds / 1e6 << " million guest instructions/s, "
              << jit.nativeInstructions << " native; checksum=" << checksum << '\n';
    return seconds;
}
} // namespace pc
