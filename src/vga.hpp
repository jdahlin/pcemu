#pragma once
#include <array>
#include <cstdint>
#include <vector>
namespace pc {
struct VGA {
    std::array<std::array<uint8_t, 65536>, 4> planes{};
    uint8_t seq[8]{}, gfx[16]{}, crtc[32]{}, attr[32]{}, latch[4]{};
    uint8_t seqIndex = 0, gfxIndex = 0, crtcIndex = 0, attrIndex = 0;
    bool attrData = false, graphics = false;
    uint8_t dac[256][3]{}, dacMask = 255, dacRead = 0, dacWrite = 0, dacComponent = 0;
    unsigned width = 640, height = 480;
    uint8_t misc = 0x67;
    static constexpr uint32_t lfbBase = 0xe0000000, vramSize = 16 * 1024 * 1024;
    static constexpr unsigned maxWidth = 1920, maxHeight = 1200;
    std::vector<uint8_t> vram = std::vector<uint8_t>(vramSize);
    uint16_t dispiIndex = 0;
    std::array<uint16_t, 11> dispi{0xb0c5, 640, 480, 8, 0, 0, 640, 0, 0, 0, 256};
    uint16_t dispiRead() const;
    void dispiWrite(uint16_t value);
    VGA();
    void mode(unsigned mode, bool clear);
    void updateGeometry();
    uint8_t read(uint32_t address);
    void write(uint32_t address, uint8_t value);
    uint8_t in(uint16_t port);
    void out(uint16_t port, uint8_t value);
    uint32_t pixel(unsigned x, unsigned y) const;
};
} // namespace pc
