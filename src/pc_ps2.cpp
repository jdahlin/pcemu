#include "machine.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <stdexcept>
namespace pc {
void Machine::key(uint16_t k) {
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
            scanCodes.push_back(0x2a);
        scanCodes.push_back(scan);
        scanCodes.push_back(scan | 0x80);
        if (shift)
            scanCodes.push_back(0xaa);
    }
    waitingKey = false;
    cpu.halted = false;
}
void Machine::keyboardPoll() {
    // No output-buffer/IRQ work is possible until queued input is ready or
    // the mouse sample deadline arrives. Inspect live state so guest I/O,
    // host input and restored snapshots need no cached-deadline invalidation.
    if (!keyboardFull && !mouse.packetDue(cpu.instructions) &&
        (cpu.instructions < keyboardNextAt ||
         (keyboardReplies.empty() && mouse.replies.empty() && scanCodes.empty() &&
          mouse.packets.empty())))
        return;
    mouse.poll(cpu.instructions);
    // Backpressure the injected scan stream until the current IRQ service ends.
    // Command replies can still arrive while a handler is waiting for an ACK.
    if (!keyboardFull && cpu.instructions >= keyboardNextAt) {
        std::deque<uint8_t> *queue = nullptr;
        bool auxiliary = false;
        if (!keyboardReplies.empty())
            queue = &keyboardReplies;
        else if (!mouse.replies.empty()) {
            queue = &mouse.replies;
            auxiliary = true;
        } else if (keyboardEnabled && !keyboardParameter && !(keyboardCommandByte & 0x10) &&
                   !(pic[0].isr & 2) && (!(keyboardCommandByte & 1) || !(pic[0].mask & 2)) &&
                   !scanCodes.empty())
            queue = &scanCodes;
        else if (!(keyboardCommandByte & 0x20) && !(pic[1].isr & 0x10) &&
                 (!(keyboardCommandByte & 2) || !(pic[1].mask & 0x10)) && !mouse.packets.empty()) {
            queue = &mouse.packets;
            auxiliary = true;
        }
        if (queue) {
            keyboardData = queue->front();
            queue->pop_front();
            keyboardAux = auxiliary;
            keyboardFull = true;
        }
    }
    if (keyboardFull && keyboardAux && (keyboardCommandByte & 2) && !mouseLine) {
        pic[1].raise(4);
        mouseLine = true;
    } else if (keyboardFull && !keyboardAux && (keyboardCommandByte & 1) && !keyLine) {
        pic[0].raise(1);
        keyLine = true;
    }
}
void Machine::keyboardCommand(uint8_t value) {
    keyboardLastCommand = true;
    keyboardControllerPending = 0;
    // AT controller F0..FF pulse low the output bits whose command bits are
    // zero. Output bit 0 is RESET#, so every even pulse command resets the CPU.
    if ((value & 0xf0) == 0xf0 && !(value & 1)) {
        resetRequested = true;
        return;
    }
    switch (value) {
    case 0x20:
        keyboardReplies.push_back(keyboardCommandByte);
        break;
    case 0x60:
    case 0xd1:
    case 0xd2:
    case 0xd3:
    case 0xd4:
        keyboardControllerPending = value;
        break;
    case 0xaa:
        keyboardCommandByte |= 4;
        keyboardReplies.push_back(0x55);
        break;
    case 0xab:
        keyboardReplies.push_back(0);
        break;
    case 0xad:
        keyboardCommandByte |= 0x10;
        break;
    case 0xae:
        keyboardCommandByte &= ~0x10;
        break;
    case 0xa7:
        keyboardCommandByte |= 0x20;
        break;
    case 0xa8:
        keyboardCommandByte &= ~0x20;
        break;
    case 0xa9:
        keyboardReplies.push_back(0);
        break;
    case 0xc0:
        keyboardReplies.push_back(0x80);
        break;
    case 0xd0:
        keyboardReplies.push_back((keyboardOutputPort & ~2) | (cpu.a20 ? 2 : 0));
        break;
    default:
        break;
    }
}
void Machine::keyboardWrite(uint8_t value) {
    keyboardLastCommand = false;
    if (keyboardControllerPending) {
        auto command = keyboardControllerPending;
        keyboardControllerPending = 0;
        if (command == 0x60) {
            keyboardCommandByte = value;
            if (!(value & 1))
                keyLine = false;
            if (!(value & 2))
                mouseLine = false;
        } else if (command == 0xd1) {
            keyboardOutputPort = value;
            cpu.a20 = value & 2;
            if (!(value & 1))
                resetRequested = true;
        } else if (command == 0xd2)
            keyboardReplies.push_back(value);
        else if (command == 0xd3)
            mouse.replies.push_back(value);
        else if (command == 0xd4)
            mouse.command(value);
        return;
    }
    if (keyboardParameter) {
        auto parameter = keyboardParameter;
        keyboardParameter = 0;
        keyboardReplies.push_back(0xfa);
        if (parameter == 0xed)
            keyboardLEDs = value & 7;
        if (parameter == 0xf0) {
            if (!value)
                keyboardReplies.push_back(keyboardScanSet);
            else if (value <= 3)
                keyboardScanSet = value;
        }
        return;
    }
    switch (value) {
    case 0xed:
    case 0xf0:
    case 0xf3:
        keyboardParameter = value;
        keyboardReplies.push_back(0xfa);
        break;
    case 0xee:
        keyboardReplies.push_back(0xee);
        break;
    case 0xf2:
        keyboardReplies.insert(keyboardReplies.end(), {0xfa, 0xab, 0x83});
        break;
    case 0xf4:
        keyboardEnabled = true;
        keyboardReplies.push_back(0xfa);
        break;
    case 0xf5:
        keyboardEnabled = false;
        keyboardLEDs = 0;
        keyboardScanSet = 2;
        keyboardReplies.push_back(0xfa);
        break;
    case 0xf6:
        keyboardLEDs = 0;
        keyboardScanSet = 2;
        keyboardReplies.push_back(0xfa);
        break;
    case 0xff:
        keyboardLEDs = 0;
        keyboardScanSet = 2;
        keyboardEnabled = true;
        keyboardReplies.insert(keyboardReplies.end(), {0xfa, 0xaa});
        break;
    case 0xfe:
        keyboardReplies.push_back(keyboardData);
        break;
    default:
        keyboardReplies.push_back(value >= 0xf7 ? 0xfa : 0xfe);
        break;
    }
}
uint32_t Machine::readKeyboardPort(uint16_t p) {
    if (p == 0x64) {
        keyboardPoll();
        return (keyboardFull ? 1 : 0) | (keyboardFull && keyboardAux ? 0x20 : 0) |
               (keyboardCommandByte & 4) | (keyboardLastCommand ? 8 : 0);
    }
    if (p == 0x60) {
        keyboardPoll();
        if (verbose)
            std::fprintf(stderr, "KBC read %02x full=%d at %04x:%08x count=%llu\n", keyboardData,
                         keyboardFull, cpu.s[CS].selector, cpu.ip,
                         static_cast<unsigned long long>(cpu.instructions));
        if (keyboardFull) {
            if (keyboardAux) {
                mouse.lastSent = keyboardData;
                mouseLine = false;
            } else
                keyLine = false;
            keyboardFull = false;
            // About one PS/2 byte at the current instruction-clock ratio.
            keyboardNextAt = cpu.instructions + 4000;
        }
        return keyboardData;
    }
    return 0;
}

} // namespace pc
