#include "io_port_space.hpp"
#include <iostream>
#include <stdexcept>
using namespace pc;
static void check(bool condition) {
    if (!condition) throw std::runtime_error("I/O port routing check failed");
}
int main() {
    IoPortSpace ports;
    unsigned reads = 0, writes = 0;
    uint16_t seenPort = 0;
    int seenWidth = 0;
    uint32_t seenValue = 0;
    auto read = [&](uint16_t port, int width) {
        ++reads; seenPort = port; seenWidth = width;
        return 0xffffffffu;
    };
    auto write = [&](uint16_t port, uint32_t value, int width) {
        ++writes; seenPort = port; seenValue = value; seenWidth = width;
    };
    ports.map("data", 0x1f0, 8, read, write);
    ports.map("control", 0x3f6, 1, read, write);
    unsigned transactions = 0;
    for (int width : {1, 2, 4}) {
        ++transactions;
        check(ports.read(0x1f0, width) == 0xffffffffu);
        check(reads == transactions && seenPort == 0x1f0 && seenWidth == width);
        check(ports.write(0x1f0, 0x12345678, width));
        check(seenValue == 0x12345678 && seenWidth == width);
    }
    check(writes == 3);
    check(ports.read(0x1f7, 4).has_value() && seenPort == 0x1f7);
    check(ports.read(0x3f6, 4).has_value() && seenPort == 0x3f6);
    for (uint16_t port : {0x1ef, 0x1f8, 0x3f5, 0x3f7}) {
        check(!ports.read(port, 1));
        check(!ports.write(port, 0, 1));
    }
    auto rejected = [&](uint16_t base, uint32_t size) {
        try { ports.map("bad", base, size, read, write); }
        catch (const std::invalid_argument &) { return true; }
        return false;
    };
    check(rejected(0x1f7, 2) && rejected(0x1ef, 2) && rejected(0x1ef, 10));
    check(rejected(0, 0) && rejected(0xffff, 2));
    ports.map("last", 0xffff, 1, read, write);
    check(ports.read(0xffff, 4).has_value());
    ports.clear();
    check(!ports.read(0x1f0, 1) && !ports.write(0xffff, 0, 1));
    ports.map("rebound", 0x1f0, 8, [](uint16_t, int) { return 7; }, write);
    check(ports.read(0x1f0, 2) == 7);
    std::cout << "Passed I/O region boundaries, widths, overlap and rebinding checks\n";
}
