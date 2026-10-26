#pragma once
#include <cstdint>
namespace pc {
// System control port B (0x61) connects software to the timer and speaker controls.
// Bit 0 gates timer counter 2; bit 1 normally enables its output at the speaker.
// Reads include the refresh signal (bit 4) and counter-2 OUT (bit 5). The board
// samples timer pins and applies the returned gate level; this device stores the
// output latch. It does not generate audio or deliver non-maskable interrupts.
// Reference: Intel 82371AB PIIX4, 290562-001 (April 1997), 4.2.4.1 NMISC, page 83.
// https://mark-ogden.uk/files/intel/publications/290562%2082371AB%20PCI-TO-ISA_IDE%20Xcelerator%20PIIX4-Apr97.pdf#page=83
// Register reference only; this header-only model retains existing PC/AT behavior.
class SystemPortBDevice {
  public:
    // Timer Counter 2 Enable: bit 0 supplies the speaker counter's GATE input.
    static constexpr uint8_t TimerCounter2Enable = 1u << 0;
    // Refresh Cycle Toggle: bit 4 reports refresh activity to diagnostic software.
    static constexpr uint8_t RefreshCycleToggle = 1u << 4;
    // Timer Counter 2 OUT Status: bit 5 reports the speaker counter's current output.
    static constexpr uint8_t TimerCounter2Output = 1u << 5;
    // Replace only timer status bits when reading the snapshot-compatible stored latch.
    static constexpr uint8_t StoredLatchReadMask =
        uint8_t(~(RefreshCycleToggle | TimerCounter2Output));
    // Reset the software latch; timer state is reset independently by the board.
    // IMPLEMENTATION NOTE: Board lifecycle, with NMISC reset value zero.
    // https://mark-ogden.uk/files/intel/publications/290562%2082371AB%20PCI-TO-ISA_IDE%20Xcelerator%20PIIX4-Apr97.pdf#page=83
    void reset() {
        latch_ = 0;
    }
    // Restore the snapshot latch without causing a new guest write or gate transition.
    // IMPLEMENTATION NOTE: Snapshot policy around NMISC storage.
    // https://mark-ogden.uk/files/intel/publications/290562%2082371AB%20PCI-TO-ISA_IDE%20Xcelerator%20PIIX4-Apr97.pdf#page=83
    void restore(uint8_t value) {
        latch_ = value;
    }
    // Read stored controls with timer-status bits sampled from board-provided signals.
    // https://mark-ogden.uk/files/intel/publications/290562%2082371AB%20PCI-TO-ISA_IDE%20Xcelerator%20PIIX4-Apr97.pdf#page=83
    // COMPATIBILITY: Bit 4 follows counter-1 OUT rather than a separate refresh-cycle latch.
    // FIXME: Implement real NMI error status/clearing; bits 6/7 currently echo the latch.
    uint8_t read_register(bool timer1, bool timer2) const {
        return (latch_ & StoredLatchReadMask) | (timer1 ? RefreshCycleToggle : 0) |
               (timer2 ? TimerCounter2Output : 0);
    }
    // Store controls and return the counter-2 enable level for board wiring.
    // https://mark-ogden.uk/files/intel/publications/290562%2082371AB%20PCI-TO-ISA_IDE%20Xcelerator%20PIIX4-Apr97.pdf#page=83
    bool write_register(uint8_t value) {
        latch_ = value;
        return value & TimerCounter2Enable;
    }

  private:
    uint8_t latch_ = 0;
};
} // namespace pc
