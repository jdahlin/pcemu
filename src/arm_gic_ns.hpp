#pragma once
#include <cstdint>
#include <functional>
namespace pc {
// Nonsecure, affinity-routed distributor view over an EL1-only native GIC.
// The host owns group assignment: every exposed interrupt must be Group 1.
// No secure-world or nested interrupt controller is exposed to the guest.
class ArmGicNonsecureView {
  public:
    // IMPLEMENTATION NOTE: Board address mappings. The guest Distributor is
    // trapped here; the native backend lives at a hidden host-selected GPA.
    static constexpr uint64_t GuestDistributorBase = 0x08000000,
                              NativeDistributorBase = 0xf00000000ull;
    using Read = std::function<uint64_t(uint32_t)>;
    using Write = std::function<void(uint32_t, uint64_t)>;
    // Require a single-state affinity-routed backend; cache its capabilities.
    ArmGicNonsecureView(Read read, Write write);
    // Return filtered Non-secure state; throw for malformed accesses.
    uint64_t read_register(uint64_t offset, unsigned width) const;
    // Forward supported SPI state and Group1 enable; ignore hidden registers.
    void write_register(uint64_t offset, uint64_t value, unsigned width);
    // Return the upper exclusive bound on ordinary interrupt IDs, including private IDs.
    // Arm GICD_TYPER.ITLinesNumber.
    // https://df.lth.se/~getz/ARM/SysReg/ext-gicd_typer.html
    unsigned interrupt_count() const {
        return interrupts;
    }

  private:
    Read load;
    Write store;
    uint32_t typer;
    unsigned interrupts;
    static void validate_access(uint64_t offset, unsigned width);
    uint32_t shared_interrupt_bitmap_mask(unsigned first) const;
};
} // namespace pc
