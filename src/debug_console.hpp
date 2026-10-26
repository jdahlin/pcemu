#pragma once
#include <cstdint>
#include <functional>
#include <utility>
namespace pc {
// The byte debug port at 0xe9 lets firmware/OS code print before configuring a
// serial console. It is an emulator convention, not a physical ISA peripheral.
// The board supplies a host output callback. Guest writes update a byte latch
// and call that sink; snapshots preserve the latch but not the host callback.
// Reference: Bochs User Manual, 4.3.46 port_e9_hack (output convention).
// https://bochs.sourceforge.io/doc/docbook/user/bochsrc.html#:~:text=4.3.46.%20port_e9_hack
// COMPATIBILITY: Our reads return the last byte, unlike Bochs' fixed 0xe9 probe.
class DebugConsoleDevice {
  public:
    using Output = std::function<void(uint8_t)>;
    // Bind the host byte sink without changing the guest-visible latch.
    // IMPLEMENTATION NOTE: Runtime wiring around the output convention.
    // https://bochs.sourceforge.io/doc/docbook/user/bochsrc.html#:~:text=4.3.46.%20port_e9_hack
    void bind_output_sink(Output output) {
        output_ = std::move(output);
    }
    // Clear the byte latch at machine reset; retain the host sink binding.
    // IMPLEMENTATION NOTE: Emulator lifecycle, not specified by the convention.
    // https://bochs.sourceforge.io/doc/docbook/user/bochsrc.html#:~:text=4.3.46.%20port_e9_hack
    void reset() {
        latch_ = 0;
    }
    // Restore the latch without printing a second copy of the saved byte.
    // IMPLEMENTATION NOTE: Snapshot lifecycle, not a guest port write.
    // https://bochs.sourceforge.io/doc/docbook/user/bochsrc.html#:~:text=4.3.46.%20port_e9_hack
    void restore(uint8_t value) {
        latch_ = value;
    }
    // Return the last written byte (PCEmu compatibility policy).
    // https://bochs.sourceforge.io/doc/docbook/user/bochsrc.html#:~:text=4.3.46.%20port_e9_hack
    uint8_t read_data_register() const {
        return latch_;
    }
    // Latch and forward one guest byte to the host console, if a sink is bound.
    // https://bochs.sourceforge.io/doc/docbook/user/bochsrc.html#:~:text=4.3.46.%20port_e9_hack
    void write_data_register(uint8_t value) {
        latch_ = value;
        if (output_)
            output_(value);
    }

  private:
    uint8_t latch_ = 0;
    Output output_;
};
} // namespace pc
