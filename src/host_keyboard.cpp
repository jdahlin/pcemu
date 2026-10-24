#include "host_keyboard.hpp"
namespace pc {
static uint16_t pcScan(SDL_Scancode s) {
    static const uint8_t letters[] = {0x1e, 0x30, 0x2e, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17,
                                      0x24, 0x25, 0x26, 0x32, 0x31, 0x18, 0x19, 0x10, 0x13,
                                      0x1f, 0x14, 0x16, 0x2f, 0x11, 0x2d, 0x15, 0x2c};
    if (s >= SDL_SCANCODE_A && s <= SDL_SCANCODE_Z)
        return letters[s - SDL_SCANCODE_A];
    if (s >= SDL_SCANCODE_1 && s <= SDL_SCANCODE_0)
        return 2 + s - SDL_SCANCODE_1;
    if (s >= SDL_SCANCODE_F1 && s <= SDL_SCANCODE_F10)
        return 0x3b + s - SDL_SCANCODE_F1;
    switch (s) {
    case SDL_SCANCODE_ESCAPE:
        return 1;
    case SDL_SCANCODE_MINUS:
        return 12;
    case SDL_SCANCODE_EQUALS:
        return 13;
    case SDL_SCANCODE_BACKSPACE:
        return 14;
    case SDL_SCANCODE_TAB:
        return 15;
    case SDL_SCANCODE_LEFTBRACKET:
        return 26;
    case SDL_SCANCODE_RIGHTBRACKET:
        return 27;
    case SDL_SCANCODE_RETURN:
        return 28;
    case SDL_SCANCODE_LCTRL:
        return 29;
    case SDL_SCANCODE_RCTRL:
        return 0xe01d;
    case SDL_SCANCODE_SEMICOLON:
        return 39;
    case SDL_SCANCODE_APOSTROPHE:
        return 40;
    case SDL_SCANCODE_GRAVE:
        return 41;
    case SDL_SCANCODE_LSHIFT:
        return 42;
    case SDL_SCANCODE_BACKSLASH:
        return 43;
    case SDL_SCANCODE_COMMA:
        return 51;
    case SDL_SCANCODE_PERIOD:
        return 52;
    case SDL_SCANCODE_SLASH:
        return 53;
    case SDL_SCANCODE_RSHIFT:
        return 54;
    case SDL_SCANCODE_LALT:
        return 56;
    case SDL_SCANCODE_RALT:
        return 0xe038;
    case SDL_SCANCODE_SPACE:
        return 57;
    case SDL_SCANCODE_CAPSLOCK:
        return 58;
    case SDL_SCANCODE_HOME:
        return 0xe000 | 71;
    case SDL_SCANCODE_UP:
        return 0xe000 | 72;
    case SDL_SCANCODE_PAGEUP:
        return 0xe000 | 73;
    case SDL_SCANCODE_LEFT:
        return 0xe000 | 75;
    case SDL_SCANCODE_RIGHT:
        return 0xe000 | 77;
    case SDL_SCANCODE_END:
        return 0xe000 | 79;
    case SDL_SCANCODE_DOWN:
        return 0xe000 | 80;
    case SDL_SCANCODE_PAGEDOWN:
        return 0xe000 | 81;
    case SDL_SCANCODE_INSERT:
        return 0xe000 | 82;
    case SDL_SCANCODE_DELETE:
        return 0xe000 | 83;
    case SDL_SCANCODE_F11:
        return 87;
    case SDL_SCANCODE_F12:
        return 88;
    case SDL_SCANCODE_LGUI:
        return 57435;
    case SDL_SCANCODE_RGUI:
        return 57436;
    case SDL_SCANCODE_APPLICATION:
        return 57437;
    case SDL_SCANCODE_KP_ENTER:
        return 57372;
    case SDL_SCANCODE_KP_DIVIDE:
        return 57397;
    case SDL_SCANCODE_KP_MULTIPLY:
        return 55;
    case SDL_SCANCODE_KP_MINUS:
        return 74;
    case SDL_SCANCODE_KP_PLUS:
        return 78;
    case SDL_SCANCODE_NUMLOCKCLEAR:
        return 69;
    case SDL_SCANCODE_SCROLLLOCK:
        return 70;
    case SDL_SCANCODE_KP_7:
        return 71;
    case SDL_SCANCODE_KP_8:
        return 72;
    case SDL_SCANCODE_KP_9:
        return 73;
    case SDL_SCANCODE_KP_4:
        return 75;
    case SDL_SCANCODE_KP_5:
        return 76;
    case SDL_SCANCODE_KP_6:
        return 77;
    case SDL_SCANCODE_KP_1:
        return 79;
    case SDL_SCANCODE_KP_2:
        return 80;
    case SDL_SCANCODE_KP_3:
        return 81;
    case SDL_SCANCODE_KP_0:
        return 82;
    case SDL_SCANCODE_KP_PERIOD:
        return 83;
    case SDL_SCANCODE_NONUSBACKSLASH:
        return 86;
    default:
        return 0;
    }
}
void HostKeyboard::emit(SDL_Scancode key, bool up) {
    if (key == SDL_SCANCODE_PAUSE) {
        if (!up)
            destination.insert(destination.end(), {0xe1, 0x1d, 0x45, 0xe1, 0x9d, 0xc5});
    } else if (key == SDL_SCANCODE_PRINTSCREEN) {
        if (up)
            destination.insert(destination.end(), {0xe0, 0xb7, 0xe0, 0xaa});
        else
            destination.insert(destination.end(), {0xe0, 0x2a, 0xe0, 0x37});
    } else if (auto scan = pcScan(key)) {
        if (scan >> 8)
            destination.push_back(scan >> 8);
        destination.push_back(uint8_t(scan) | (up ? 0x80 : 0));
    }
}
void HostKeyboard::event(SDL_Scancode key, bool up) {
    if (key < 0 || key >= SDL_NUM_SCANCODES)
        return;
    bool command = key == SDL_SCANCODE_LGUI || key == SDL_SCANCODE_RGUI;
    if (up) {
        if (!held[key])
            return;
        // A bare Command tap is a guest Windows-key tap. Delay its make so
        // native Cmd+G/Cmd+Q and Cmd+Tab don't open the guest Start menu.
        if (command && !sent[key])
            emit(key, false);
        if (sent[key] || command)
            emit(key, true);
        held[key] = sent[key] = false;
        return;
    }
    held[key] = true;
    if (command)
        return;
    for (auto meta : {SDL_SCANCODE_LGUI, SDL_SCANCODE_RGUI})
        if (held[meta] && !sent[meta]) {
            emit(meta, false);
            sent[meta] = true;
        }
    emit(key, false);
    sent[key] = true;
}
void HostKeyboard::release() {
    for (unsigned i = 0; i < held.size(); ++i) {
        if (sent[i])
            emit(SDL_Scancode(i), true);
        held[i] = sent[i] = false;
    }
}
void HostKeyboard::consumeCommand() {
    for (auto key : {SDL_SCANCODE_LGUI, SDL_SCANCODE_RGUI}) {
        if (sent[key])
            emit(key, true);
        held[key] = sent[key] = false;
    }
}
} // namespace pc
