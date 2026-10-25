#include "host_keyboard.hpp"
#include "host_key.hpp"
namespace pc {
static uint16_t pcScan(uint16_t s) {
    static const uint8_t letters[] = {0x1e, 0x30, 0x2e, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17,
                                      0x24, 0x25, 0x26, 0x32, 0x31, 0x18, 0x19, 0x10, 0x13,
                                      0x1f, 0x14, 0x16, 0x2f, 0x11, 0x2d, 0x15, 0x2c};
    if (s >= hid::KEY_A && s <= hid::KEY_Z)
        return letters[s - hid::KEY_A];
    if (s >= hid::KEY_1 && s <= hid::KEY_0)
        return 2 + s - hid::KEY_1;
    if (s >= hid::KEY_F1 && s <= hid::KEY_F10)
        return 0x3b + s - hid::KEY_F1;
    switch (s) {
    case hid::KEY_ESCAPE:
        return 1;
    case hid::KEY_MINUS:
        return 12;
    case hid::KEY_EQUALS:
        return 13;
    case hid::KEY_BACKSPACE:
        return 14;
    case hid::KEY_TAB:
        return 15;
    case hid::KEY_LEFTBRACKET:
        return 26;
    case hid::KEY_RIGHTBRACKET:
        return 27;
    case hid::KEY_RETURN:
        return 28;
    case hid::KEY_LCTRL:
        return 29;
    case hid::KEY_RCTRL:
        return 0xe01d;
    case hid::KEY_SEMICOLON:
        return 39;
    case hid::KEY_APOSTROPHE:
        return 40;
    case hid::KEY_GRAVE:
        return 41;
    case hid::KEY_LSHIFT:
        return 42;
    case hid::KEY_BACKSLASH:
        return 43;
    case hid::KEY_COMMA:
        return 51;
    case hid::KEY_PERIOD:
        return 52;
    case hid::KEY_SLASH:
        return 53;
    case hid::KEY_RSHIFT:
        return 54;
    case hid::KEY_LALT:
        return 56;
    case hid::KEY_RALT:
        return 0xe038;
    case hid::KEY_SPACE:
        return 57;
    case hid::KEY_CAPSLOCK:
        return 58;
    case hid::KEY_HOME:
        return 0xe000 | 71;
    case hid::KEY_UP:
        return 0xe000 | 72;
    case hid::KEY_PAGEUP:
        return 0xe000 | 73;
    case hid::KEY_LEFT:
        return 0xe000 | 75;
    case hid::KEY_RIGHT:
        return 0xe000 | 77;
    case hid::KEY_END:
        return 0xe000 | 79;
    case hid::KEY_DOWN:
        return 0xe000 | 80;
    case hid::KEY_PAGEDOWN:
        return 0xe000 | 81;
    case hid::KEY_INSERT:
        return 0xe000 | 82;
    case hid::KEY_DELETE:
        return 0xe000 | 83;
    case hid::KEY_F11:
        return 87;
    case hid::KEY_F12:
        return 88;
    case hid::KEY_LGUI:
        return 57435;
    case hid::KEY_RGUI:
        return 57436;
    case hid::KEY_APPLICATION:
        return 57437;
    case hid::KEY_KP_ENTER:
        return 57372;
    case hid::KEY_KP_DIVIDE:
        return 57397;
    case hid::KEY_KP_MULTIPLY:
        return 55;
    case hid::KEY_KP_MINUS:
        return 74;
    case hid::KEY_KP_PLUS:
        return 78;
    case hid::KEY_NUMLOCKCLEAR:
        return 69;
    case hid::KEY_SCROLLLOCK:
        return 70;
    case hid::KEY_KP_7:
        return 71;
    case hid::KEY_KP_8:
        return 72;
    case hid::KEY_KP_9:
        return 73;
    case hid::KEY_KP_4:
        return 75;
    case hid::KEY_KP_5:
        return 76;
    case hid::KEY_KP_6:
        return 77;
    case hid::KEY_KP_1:
        return 79;
    case hid::KEY_KP_2:
        return 80;
    case hid::KEY_KP_3:
        return 81;
    case hid::KEY_KP_0:
        return 82;
    case hid::KEY_KP_PERIOD:
        return 83;
    case hid::KEY_NONUSBACKSLASH:
        return 86;
    default:
        return 0;
    }
}
void HostKeyboard::emit(uint16_t key, bool up) {
    if (key == hid::KEY_PAUSE) {
        if (!up)
            emitBytes({0xe1, 0x1d, 0x45, 0xe1, 0x9d, 0xc5});
    } else if (key == hid::KEY_PRINTSCREEN) {
        if (up)
            emitBytes({0xe0, 0xb7, 0xe0, 0xaa});
        else
            emitBytes({0xe0, 0x2a, 0xe0, 0x37});
    } else if (auto scan = pcScan(key)) {
        if (scan >> 8)
            destination(scan >> 8);
        destination(uint8_t(scan) | (up ? 0x80 : 0));
    }
}
} // namespace pc
