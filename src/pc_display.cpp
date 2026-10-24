#include "pc_display.hpp"
#include "machine.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>
namespace pc {
namespace {
const uint32_t palette[16] = {0xff000000, 0xff0000aa, 0xff00aa00, 0xff00aaaa,
                                      0xffaa0000, 0xffaa00aa, 0xffaa5500, 0xffaaaaaa,
                                      0xff555555, 0xff5555ff, 0xff55ff55, 0xff55ffff,
                                      0xffff5555, 0xffff55ff, 0xffffff55, 0xffffffff};
}
void PcDisplay::loadFont(std::span<const uint8_t> bytes) {
    if (bytes.empty())
        return;
    const uint8_t glyph[] = {0x30, 0x78, 0xcc, 0xcc, 0xfc, 0xcc, 0xcc, 0};
    auto it = std::search(bytes.begin(), bytes.end(), std::begin(glyph), std::end(glyph));
    if (it == bytes.end() || it - bytes.begin() < 65 * 8)
        throw std::runtime_error("Cannot locate 8x8 font in VGA ROM");
    size_t off = it - bytes.begin() - 65 * 8;
    std::copy_n(bytes.begin() + off, 2048, font.begin());
}
void PcDisplay::refresh(Machine &m, bool executeVga) {
    auto &c = m.cpu;
    m.vga.updateGeometry();
    if (!executeVga)
        return;
    auto mode = c.mem[0x449] & 127;
    m.videoMode = mode;
    unsigned columns = c.mem[0x44a] | (c.mem[0x44b] << 8);
    if (columns == 40 || columns == 80)
        m.columns = columns;
    m.activePage = c.mem[0x462] & 7;
    m.rows = std::min(50u, unsigned(c.mem[0x484]) + 1);
    if (!m.rows)
        m.rows = 25;
    auto pos = c.mem[0x450 + m.activePage * 2] | (c.mem[0x451 + m.activePage * 2] << 8);
    m.cursor = (pos >> 8) * m.columns + (pos & 255);
}
void PcDisplay::save(Machine &m, const std::string &framePath, bool verbose) const {
    auto &c = m.cpu;
    if (verbose) {
        std::cerr << "VGA attribute palette:";
        for (int k = 0; k < 16; k++)
            std::cerr << ' ' << unsigned(m.vga.attr[k]);
        std::cerr << " DAC1=" << unsigned(m.vga.dac[1][0]) << ','
                  << unsigned(m.vga.dac[1][1]) << ',' << unsigned(m.vga.dac[1][2]) << '\n';
    }
    if (framePath.empty())
        return;
    unsigned width = m.vga.graphics ? m.vga.width : 640,
             height = m.vga.graphics ? m.vga.height : 400;
    std::ofstream f(framePath, std::ios::binary);
    f << "P6\n" << width << ' ' << height << "\n255\n";
    std::vector<uint8_t> rgb(size_t(width) * height * 3);
    for (unsigned y = 0; y < height; y++)
        for (unsigned x = 0; x < width; x++) {
            uint32_t px;
            if (m.vga.graphics)
                px = m.vga.pixel(x, y);
            else {
                auto off =
                    0xb8000 + m.activePage * 4096 + ((y / 16) * m.columns + x / 8) * 2;
                auto ch = c.mem[off], attr = c.mem[off + 1];
                bool on = font[ch * 8 + (y % 16) / 2] & (128 >> (x & 7));
                px = palette[on ? (attr & 15) : (attr >> 4) & 7];
            }
            auto at = (size_t(y) * width + x) * 3;
            rgb[at] = px >> 16;
            rgb[at + 1] = px >> 8;
            rgb[at + 2] = px;
        }
    f.write(reinterpret_cast<const char *>(rgb.data()), rgb.size());
}
void PcDisplay::render(Machine &m, std::span<uint32_t> pixels, unsigned displayWidth,
                       unsigned displayHeight, uint64_t frameTicks) const {
    auto &c = m.cpu;
    for (unsigned y = 0; y < displayHeight; y++)
        for (unsigned x = 0; x < displayWidth; x++) {
            if (m.vga.graphics) {
                pixels[y * displayWidth + x] = m.vga.pixel(x, y);
                continue;
            }
            unsigned col = x / 8, row = y / 16;
            uint32_t px = palette[0];
            if (col < m.columns && row < m.rows) {
                auto off = 0xb8000 + m.activePage * 4096 + (row * m.columns + col) * 2;
                uint8_t ch = c.mem[off], attr = c.mem[off + 1],
                        bits = font[ch * 8 + (y % 16) / 2];
                px = palette[(bits & (128 >> (x % 8))) ? (attr & 15)
                                                       : ((attr >> 4) & 7)];
                if (row * m.columns + col == m.cursor && y % 16 >= 14 &&
                    (frameTicks / 500) % 2 == 0)
                    px = palette[7];
            }
            pixels[y * displayWidth + x] = px;
        }
}
} // namespace pc
