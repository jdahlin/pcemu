#include "machine.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <stdexcept>
#include <cstdlib>
namespace pc {
void Machine::scroll(unsigned top, unsigned left, unsigned bottom, unsigned right, int lines,
                     uint8_t attr) {
    top = std::min(top, rows - 1);
    bottom = std::min(bottom, rows - 1);
    left = std::min(left, columns - 1);
    right = std::min(right, columns - 1);
    if (top > bottom || left > right)
        return;
    int n = std::abs(lines), height = bottom - top + 1;
    if (!n || n > height)
        n = height;
    for (int yy = 0; yy < height; yy++) {
        int y = lines < 0 ? bottom - yy : top + yy, src = lines < 0 ? y - n : y + n;
        for (unsigned x = left; x <= right; x++) {
            auto v = src >= int(top) && src <= int(bottom)
                         ? cpu.read(0xb8000 + (src * columns + x) * 2 + activePage * 4096, 2)
                         : (uint32_t(attr) << 8) | 32;
            cpu.write(0xb8000 + (y * columns + x) * 2 + activePage * 4096, v, 2);
        }
    }
}
void Machine::character(uint8_t c, uint8_t attr, bool tty) {
    if (tty) {
        if (c == 13)
            cursor -= cursor % columns;
        else if (c == 10)
            cursor += columns;
        else if (c == 8) {
            if (cursor)
                cursor--;
        } else if (c == 7) {
        } else {
            cpu.write(0xb8000 + activePage * 4096 + cursor * 2, c | (attr << 8), 2);
            cursor++;
        }
        if (cursor >= columns * rows) {
            scroll(0, 0, rows - 1, columns - 1, 1, attr);
            cursor -= columns;
        }
    } else
        cpu.write(0xb8000 + activePage * 4096 + cursor * 2, c | (attr << 8), 2);
    cpu.write(0x450 + activePage * 2, (cursor / columns) * 256 + cursor % columns, 2);
}
std::string Machine::text() const {
    std::string result;
    for (unsigned y = 0; y < rows; y++) {
        std::string line;
        for (unsigned x = 0; x < columns; x++) {
            auto ch = cpu.mem[0xb8000 + activePage * 4096 + (y * columns + x) * 2];
            line += ch >= 32 && ch < 127 ? char(ch) : ' ';
        }
        while (!line.empty() && line.back() == ' ')
            line.pop_back();
        result += line + '\n';
    }
    return result;
}
} // namespace pc
