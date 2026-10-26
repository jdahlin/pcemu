#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
namespace pc {
// CMOS/RTC register subset used by the PC/AT firmware and guests. The machine
// supplies installed hardware; this device owns its selected register and clock.
// IBM PC/AT Technical Reference, 6280070, September 1985 (1-56 through 1-68).
// https://minuszerodegrees.net/manuals/IBM_5170_Technical_Reference_6280070_SEP85.pdf#page=72
class CmosRtc {
  public:
    struct Config {
        size_t ramBytes;
        bool disk, floppy;
    };
    struct State {
        uint8_t index = 0;
        int64_t offset = 0;
    } state;
    explicit CmosRtc(std::function<int64_t()> clock = {});
    // IMPLEMENTATION NOTE: Bind machine-installed hardware for BIOS configuration reads.
    // https://minuszerodegrees.net/manuals/IBM_5170_Technical_Reference_6280070_SEP85.pdf#page=84
    void configure(Config hardware) {
        config = hardware;
    }
    // IMPLEMENTATION NOTE: Reset the selected index while preserving the VM clock offset.
    // https://minuszerodegrees.net/manuals/IBM_5170_Technical_Reference_6280070_SEP85.pdf#page=84
    void reset() {
        state.index = 0;
    } // Warm reset preserves the VM clock.
    // IMPLEMENTATION NOTE: Latch the port-0x70 index; bit 7 does not address a CMOS byte.
    // https://minuszerodegrees.net/manuals/IBM_5170_Technical_Reference_6280070_SEP85.pdf#page=84
    void select_register(uint8_t index) {
        state.index = index;
    }
    // Read the selected calendar, status or BIOS configuration byte.
    uint32_t read_data_register() const;
    // IMPLEMENTATION NOTE: Return host seconds plus the VM-only clock offset.
    // https://minuszerodegrees.net/manuals/IBM_5170_Technical_Reference_6280070_SEP85.pdf#page=84
    int64_t time_seconds() const {
        return seconds() + state.offset;
    }
    // IMPLEMENTATION NOTE: Set the VM clock offset without changing host wall time.
    // https://minuszerodegrees.net/manuals/IBM_5170_Technical_Reference_6280070_SEP85.pdf#page=84
    void set_time(int64_t time) {
        state.offset = time - seconds();
    }

  private:
    // IMPLEMENTATION NOTE: Default to 16 MiB until machine composition configures it.
    static constexpr size_t DefaultRamBytes = 16 * 1024 * 1024;
    Config config{DefaultRamBytes, false, false};
    std::function<int64_t()> seconds;
};
} // namespace pc
