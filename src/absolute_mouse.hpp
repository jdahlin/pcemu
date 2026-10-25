#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
namespace pc {
// VMware-compatible low-bandwidth absolute pointer protocol. The independent
// guest mouse driver uses ordinary PS/2 interrupts as queue notifications.
struct AbsoluteMouse {
    static constexpr uint16_t port = 0x5658;
    static constexpr uint32_t magic = 0x564d5868;
    std::array<uint32_t, 128> words{};
    unsigned count = 0;
    bool enabled = false, absolute = false, error = false;
    bool active() const { return enabled && absolute && !error; }
    void reset() { *this = {}; }
    bool read(int width, uint32_t *r) {
        // Architectural register order: EAX, ECX, EDX, EBX.
        if (width != 4 || r[0] != magic || uint16_t(r[2]) != port) return false;
        const auto command = uint16_t(r[1]);
        const auto argument = r[3];
        r[0] = 0xffffffff;
        switch (command) {
        case 10:
            r[0] = 6; r[3] = magic; r[1] = 0; break;
        case 41:
            if (argument == 0x45414552) {
                reset(); enabled = true; words[count++] = 0x3442554a;
            } else if (argument == 0xf5) reset();
            else if (argument == 0x53424152 && enabled) absolute = true;
            else if (argument == 0x4c455252) { absolute = false; count = 0; }
            else break;
            r[0] = 0;
            break;
        case 40:
            r[0] = enabled && !error ? count : 0xffff0000;
            break;
        case 39: {
            if (!enabled || error || !argument || argument > 4 || argument > count) {
                error = true; absolute = false; r[0] = 0xffff0000; break;
            }
            constexpr unsigned outputs[]{0, 3, 1, 2};
            for (unsigned i = 0; i < argument; ++i) r[outputs[i]] = words[i];
            std::move(words.begin() + argument, words.begin() + count, words.begin());
            count -= argument;
            break;
        }
        }
        return true;
    }
    bool motion(int x, int y, int wheel, uint8_t buttons) {
        if (!active()) return false;
        const uint32_t status = ((buttons & 1) ? 0x20 : 0) | ((buttons & 2) ? 0x10 : 0) |
                                ((buttons & 4) ? 8 : 0);
        // Coalesce only complete motion-only packets with unchanged buttons.
        // Button edges and wheel reports retain their ordering.
        if (count >= 4 && !wheel && words[count - 4] == status && !words[count - 1]) {
            words[count - 3] = std::clamp(x, 0, 65535);
            words[count - 2] = std::clamp(y, 0, 65535);
            return true;
        }
        if (count > words.size() - 4) {
            error = true; absolute = false; return false;
        }
        words[count++] = status;
        words[count++] = std::clamp(x, 0, 65535);
        words[count++] = std::clamp(y, 0, 65535);
        words[count++] = uint32_t(wheel);
        return true;
    }
};
} // namespace pc
