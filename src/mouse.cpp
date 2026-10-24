#include "mouse.hpp"
#include <algorithm>
namespace pc {
void PS2Mouse::defaults() {
    enabled = remote = wrap = scaling2 = false;
    resolution = 2;
    rate = 100;
    id = parameter = 0;
    rates = {};
    dx = dy = wheel = 0;
    nextAt = 0;
    reportedButtons = buttons;
    packets.clear();
}
uint8_t PS2Mouse::status() const {
    return (remote ? 64 : 0) | (enabled ? 32 : 0) | (scaling2 ? 16 : 0) | ((buttons & 1) << 2) |
           ((buttons & 2) >> 1) | ((buttons & 4) >> 1);
}
void PS2Mouse::command(uint8_t value) {
    if (wrap && value != 0xec && value != 0xff) {
        replies.push_back(value);
        return;
    }
    if (parameter) {
        if (parameter == 0xe8) {
            if (value > 3) {
                replies.push_back(0xfe);
                return;
            }
            resolution = value;
        } else {
            if (value != 10 && value != 20 && value != 40 && value != 60 && value != 80 &&
                value != 100 && value != 200) {
                replies.push_back(0xfe);
                return;
            }
            rate = value;
            rates = {rates[1], rates[2], value};
            if (rates == std::array<uint8_t, 3>{200, 100, 80})
                id = 3;
        }
        parameter = 0;
        replies.push_back(0xfa);
        return;
    }
    if (value == 0xfe) {
        replies.push_back(lastSent);
        return;
    }
    switch (value) {
    case 0xff:
        defaults();
        replies.clear();
        replies.insert(replies.end(), {0xfa, 0xaa, 0});
        break;
    case 0xf6:
        defaults();
        replies.push_back(0xfa);
        break;
    case 0xf4:
        enabled = true;
        replies.push_back(0xfa);
        break;
    case 0xf5:
        enabled = false;
        packets.clear();
        replies.push_back(0xfa);
        break;
    case 0xf3:
    case 0xe8:
        parameter = value;
        replies.push_back(0xfa);
        break;
    case 0xf2:
        replies.insert(replies.end(), {0xfa, id});
        break;
    case 0xf0:
        remote = true;
        packets.clear();
        replies.push_back(0xfa);
        break;
    case 0xea:
        remote = false;
        replies.push_back(0xfa);
        break;
    case 0xee:
        wrap = true;
        packets.clear();
        replies.push_back(0xfa);
        break;
    case 0xec:
        wrap = false;
        replies.push_back(0xfa);
        break;
    case 0xeb:
        replies.push_back(0xfa);
        packet(replies);
        break;
    case 0xe9:
        replies.insert(replies.end(), {0xfa, status(), resolution, rate});
        break;
    case 0xe6:
    case 0xe7:
        scaling2 = value == 0xe7;
        replies.push_back(0xfa);
        break;
    default:
        replies.push_back(0xfe);
        break;
    }
}
void PS2Mouse::packet(std::deque<uint8_t> &destination) {
    int limit = scaling2 ? 127 : 255;
    int x = std::clamp(dx, -limit, limit), y = std::clamp(dy, -limit, limit);
    dx -= x;
    dy -= y;
    auto scale = [&](int value) {
        if (!scaling2)
            return value;
        int magnitude = value < 0 ? -value : value;
        const int small[] = {0, 1, 1, 3, 6, 9};
        int result = magnitude < 6 ? small[magnitude] : magnitude * 2;
        return value < 0 ? -result : result;
    };
    x = scale(x);
    y = scale(y);
    destination.push_back(8 | buttons | (x < 0 ? 16 : 0) | (y < 0 ? 32 : 0));
    destination.push_back(uint8_t(x));
    destination.push_back(uint8_t(y));
    if (id == 3) {
        int z = std::clamp(wheel, -127, 127);
        wheel -= z;
        destination.push_back(uint8_t(z));
    } else
        wheel = 0;
    reportedButtons = buttons;
}
void PS2Mouse::motion(int x, int y, uint8_t held, int scroll) {
    bool changed = buttons != (held & 7);
    buttons = held & 7;
    if (!enabled)
        return;
    dx = int32_t(std::clamp(int64_t(dx) + x, int64_t(-32768), int64_t(32767)));
    dy = int32_t(std::clamp(int64_t(dy) + y, int64_t(-32768), int64_t(32767)));
    if (id == 3)
        wheel = int32_t(std::clamp(int64_t(wheel) + scroll, int64_t(-32768), int64_t(32767)));
    // Keep down/up transitions even when both arrive in one frontend batch.
    if (changed && !remote && !wrap && packets.size() < 512)
        packet(packets);
}
void PS2Mouse::poll(uint64_t instructions) {
    if (enabled && !remote && !wrap && packets.empty() && instructions >= nextAt &&
        (dx || dy || wheel || buttons != reportedButtons)) {
        packet(packets);
        nextAt = instructions + 4772728 / rate;
    }
}
} // namespace pc
