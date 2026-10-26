#pragma once
#include <cstdint>
namespace pc {
// System Control Port A (0x92) gives PC software a fast path to A20 and CPU reset.
// A20 is address bit 20: disabling it makes addresses wrap as on the original PC.
// Writes return requested effects; the board applies the CPU pin and reset action.
// IMPLEMENTATION NOTE: This small header-only model has no stored register state.
// Reference: Intel 82371AB PIIX4, 290562-001 (April 1997), 4.2.7.2 P92, page 87.
// https://mark-ogden.uk/files/intel/publications/290562%2082371AB%20PCI-TO-ISA_IDE%20Xcelerator%20PIIX4-Apr97.pdf#page=87
// This is a register reference; the machine does not emulate an entire PIIX4.
class Port92Device {
  public:
    // FAST_A20 bit 1 requests access through address bit 20 without wrapping.
    static constexpr uint8_t FastA20 = 1u << 1;
    // FAST_INIT bit 0 requests the board's processor-reset action.
    static constexpr uint8_t FastInitialization = 1u << 0;
    // IMPLEMENTATION NOTE: Effects describe wiring actions, not guest register storage.
    struct Effects {
        bool a20, reset;
    };
    // Report the current board A20 pin in bit 1; all other bits read zero.
    // https://mark-ogden.uk/files/intel/publications/290562%2082371AB%20PCI-TO-ISA_IDE%20Xcelerator%20PIIX4-Apr97.pdf#page=87
    // COMPATIBILITY: Sample the actual pin instead of storing an independent FAST_A20 latch.
    uint8_t read_register(bool a20) const {
        return a20 ? FastA20 : 0;
    }
    // Decode FAST_A20 and FAST_INIT into effects for the board to apply.
    // https://mark-ogden.uk/files/intel/publications/290562%2082371AB%20PCI-TO-ISA_IDE%20Xcelerator%20PIIX4-Apr97.pdf#page=87
    // FIXME: Model independent keyboard/fast A20 requests and FAST_INIT zero-to-one
    // rearming before claiming the complete PIIX4 P92 pin behavior.
    Effects write_register(uint8_t value) const {
        return {bool(value & FastA20), bool(value & FastInitialization)};
    }
};
} // namespace pc
