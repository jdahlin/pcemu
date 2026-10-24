#include "pc_input.hpp"
#include "i8042.hpp"
#include <cstring>
namespace pc {
void enqueuePcKey(I8042Device &controller, uint16_t k) {
    if (!k)
        return;
    bool shift = (k & 255) >= 'A' && (k & 255) <= 'Z';
    if (!(k >> 8)) {
        const char *ascii = "1234567890-=qwertyuiop[]asdfghjkl;'`\\zxcvbnm,./";
        const uint8_t scans[] = {2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 16, 17, 18, 19,
                                 20, 21, 22, 23, 24, 25, 26, 27, 30, 31, 32, 33, 34, 35, 36, 37,
                                 38, 39, 40, 41, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53};
        auto ch = uint8_t(k);
        auto lower = ch >= 'A' && ch <= 'Z' ? ch + 32 : ch;
        const char *shifted = "!@#$%^&*()_+{}:\"~|<>?";
        const char *plain = "1234567890-=[];'`\\,./";
        if (auto special = std::strchr(shifted, ch)) {
            lower = plain[special - shifted];
            shift = true;
        }
        auto found = std::strchr(ascii, lower);
        if (found)
            k |= uint16_t(scans[found - ascii]) << 8;
        else if (ch == ' ')
            k |= 0x3900;
        else if (ch == 13)
            k |= 0x1c00;
        else if (ch == 9)
            k |= 0x0f00;
        else if (ch == 8)
            k |= 0x0e00;
        else if (ch == 27)
            k |= 0x0100;
    }
    // Host input represents a physical keyboard even while Windows temporarily
    // leaves protected mode. BIOS input is produced only by its IRQ handler.
    auto scan = k >> 8;
    if (scan) {
        if (shift)
            controller.enqueueScan(0x2a);
        controller.enqueueScan(scan);
        controller.enqueueScan(scan | 0x80);
        if (shift)
            controller.enqueueScan(0xaa);
    }
}
} // namespace pc
