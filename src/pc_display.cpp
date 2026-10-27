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
    m.vga.update_display_geometry();
    if (!executeVga)
        return;
    auto mode = c.mem[0x449] & 127;
    m.videoMode = mode;
    unsigned columns = c.mem[0x44a] | (c.mem[0x44b] << 8);
    if (columns == 40 || columns == 80)
        m.columns = columns;
    m.activePage = c.mem[0x462] & 7;
    m.rows = std::min(256u, unsigned(c.mem[0x484]) + 1);
    if (!m.rows)
        m.rows = 25;
    auto pos = c.mem[0x450 + m.activePage * 2] | (c.mem[0x451 + m.activePage * 2] << 8);
    m.cursor = (pos >> 8) * m.columns + (pos & 255);
}
void PcDisplay::save(Machine &m, const std::string &framePath, bool verbose) const {
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
    std::vector<uint32_t> graphics;
    if (m.vga.graphics) {
        graphics.resize(size_t(width) * height);
        m.vga.render_graphics(graphics, width, height);
    }
    for (unsigned y = 0; y < height; y++)
        for (unsigned x = 0; x < width; x++) {
            uint32_t px;
            if (m.vga.graphics)
                px = graphics[size_t(y) * width + x];
            else {
                px = text_pixel(m, x, y, width, height, false, 0);
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
    if (m.vga.graphics) {
        m.vga.render_graphics(pixels, displayWidth, displayHeight);
        return;
    }
    if (displayHeight && displayWidth > pixels.size() / displayHeight)
        throw std::invalid_argument("Text frame buffer is too small");
    for (unsigned y = 0; y < displayHeight; y++)
        for (unsigned x = 0; x < displayWidth; x++)
            pixels[size_t(y) * displayWidth + x] =
                text_pixel(m, x, y, displayWidth, displayHeight, true, frameTicks);
}
// Host presentation keeps a 640x400 logical text surface, scaling 40/80-column
// and font-height/row variants into it. Guest firmware still programs real VGA
// memory/register state. Native VGA text uses plane-2 font maps and DAC colors;
// the original host BIOS's simplified preset retains its ROM-font fallback.
// IBM 42G2193 character map selection and Attribute controls, PDF 76/113/119.
// https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/42G2193_PS2_Hardware_Interface_Technical_Reference_Video_Subsystems_Sep92.pdf#page=76
uint32_t PcDisplay::text_pixel(const Machine &m, unsigned x, unsigned y,
                             unsigned width, unsigned height, bool animate, uint64_t ticks) const {
    const auto &c = m.cpu;
    const auto &v = m.vga;
    if (!width || !height || !m.columns || !m.rows || (v.seq[1] & 0x20) || !(v.attrIndex & 0x20))
        return 0xff000000;
    const bool native = !(v.seq[4] & VGA::OddEvenDisable);
    unsigned glyphHeight = native ? std::clamp(unsigned(c.mem[0x485] | (c.mem[0x486] << 8)), 1u, 32u) : 16;
    unsigned dotX = uint64_t(x) * m.columns * 8 / width;
    unsigned dotY = uint64_t(y) * m.rows * glyphHeight / height;
    unsigned col = dotX / 8, row = dotY / glyphHeight, scanline = dotY % glyphHeight;
    unsigned pageBytes = c.mem[0x44c] | (c.mem[0x44d] << 8);
    if (!pageBytes) pageBytes = 4096;
    unsigned base = (c.mem[0x449] & 127) == 7 ? 0xb0000 : 0xb8000;
    uint64_t offset = base + uint64_t(m.activePage) * pageBytes + (uint64_t(row) * m.columns + col) * 2;
    if (offset + 1 >= c.mem.size()) return 0xff000000;
    uint8_t character = c.mem[offset], attributes = c.mem[offset + 1];
    unsigned firstMap = (v.seq[3] & 3) | ((v.seq[3] >> 2) & 4);
    unsigned secondMap = ((v.seq[3] >> 2) & 3) | ((v.seq[3] >> 3) & 4);
    unsigned map = attributes & 8 ? secondMap : firstMap;
    unsigned mapOffset = ((map & 3) << 14) | ((map & 4) << 11);
    uint8_t bits = native ? v.planes[2][mapOffset + character * 32 + scanline]
                          : font[character * 8 + scanline / 2];
    bool on = bits & (128 >> (dotX & 7));
    if (native && animate && (v.attr[0x10] & 8) && (attributes & 0x80) && (ticks / 500) % 2)
        on = false;
    unsigned foreground = attributes & ((native && firstMap != secondMap) ? 7 : 15);
    unsigned background = (attributes >> 4) & ((native && !(v.attr[0x10] & 8)) ? 15 : 7);
    unsigned color = on ? foreground : background;
    unsigned shape = c.mem[0x460] | (c.mem[0x461] << 8);
    unsigned start = native ? (shape >> 8) & 31 : 14;
    unsigned end = native ? shape & 31 : 15;
    if (animate && !(shape & 0x2000) && row * m.columns + col == m.cursor &&
        scanline >= start && scanline <= end && (ticks / 500) % 2 == 0)
        color = native ? foreground : 7;
    if (!native) return palette[color];
    unsigned index = v.attribute_palette_index(color) & v.dacMask;
    auto expand = [](uint8_t component) { return unsigned((component << 2) | (component >> 4)); };
    return 0xff000000 | (expand(v.dac[index][0]) << 16) | (expand(v.dac[index][1]) << 8) |
           expand(v.dac[index][2]);
}
} // namespace pc
