#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
namespace pc {
// CMOS/RTC register subset used by the PC/AT firmware and guests. The machine
// supplies installed hardware; this device owns its selected register and clock.
class CmosRtc {
  public:
    struct Config { size_t ramBytes; bool disk, floppy; };
    struct State { uint8_t index = 0; int64_t offset = 0; } state;
    explicit CmosRtc(std::function<int64_t()> clock = {});
    void configure(Config hardware) { config = hardware; }
    void reset() { state.index = 0; } // Warm reset preserves the VM clock.
    void select(uint8_t index) { state.index = index; }
    uint32_t readData() const;
    int64_t now() const { return seconds() + state.offset; }
    void setTime(int64_t time) { state.offset = time - seconds(); }
  private:
    Config config{16 * 1024 * 1024, false, false};
    std::function<int64_t()> seconds;
};
} // namespace pc
