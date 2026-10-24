#include "cmos_rtc.hpp"
#include <ctime>
#include <utility>
namespace pc {
CmosRtc::CmosRtc(std::function<int64_t()> clock) : seconds(std::move(clock)) {
    if (!seconds) seconds = [] { return int64_t(std::time(nullptr)); };
}
uint32_t CmosRtc::readData() const {
    std::time_t current = now();
    auto t = *std::localtime(&current);
    auto bcd = [](int v) { return (v / 10) * 16 + v % 10; };
    switch (state.index & 127) {
    case 0:
        return bcd(t.tm_sec);
    case 2:
        return bcd(t.tm_min);
    case 4:
        return bcd(t.tm_hour);
    case 7:
        return bcd(t.tm_mday);
    case 8:
        return bcd(t.tm_mon + 1);
    case 9:
        return bcd(t.tm_year % 100);
    case 0xa:
        return 0x26;
    case 0xb:
        return 2;
    case 0xd:
        return 0x80;
    case 0x10:
        return config.floppy ? 0x40 : 0;
    case 0x12:
        return config.disk ? 0xf0 : 0;
    case 0x14:
        return 0x20 | (config.floppy ? 1 : 0);
    case 0x15:
        return 128;
    case 0x16:
        return 2;
    case 0x17:
    case 0x30:
        return ((config.ramBytes - 1024 * 1024) / 1024) & 255;
    case 0x18:
    case 0x31:
        return ((config.ramBytes - 1024 * 1024) / 1024) >> 8;
    default:
        return 0;
    }
}

} // namespace pc
