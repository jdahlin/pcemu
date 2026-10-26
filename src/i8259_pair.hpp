#pragma once
#include "pic.hpp"
#include <array>
#include <cstddef>
#include <optional>
namespace pc {
// PC/AT's cascaded pair of 8259A devices. CPU readiness is supplied by the
// board; priority, cascade, acknowledge and compatibility mirrors live here.
// Intel 8259A datasheet, 231468-003 (December 1988), Cascade Mode.
// https://www.pcjs.org/documents/datasheets/intel/INTEL_8259A_PIC.pdf#page=19
class I8259PairDevice {
  public:
    // Fixed PC/AT board wiring: two chips, secondary output on primary input 2.
    static constexpr unsigned ControllerCount = 2;
    // The primary chip directly supplies the CPU interrupt output.
    static constexpr unsigned PrimaryController = 0;
    // The secondary chip supplies interrupts through the primary cascade input.
    static constexpr unsigned SecondaryController = 1;
    // Primary IR2 carries all requests forwarded by the secondary chip.
    static constexpr unsigned CascadeInterrupt = 2;
    // When a request disappears during acknowledge, hardware supplies level 7.
    static constexpr unsigned SpuriousInterruptLevel = 7;
    // Snapshot-compatible initial BIOS vector base for the secondary chip.
    static constexpr uint8_t SecondaryVectorBase = 0x70;
    // Board IRQ0 is the timer input; its bit is used in legacy mirrors.
    static constexpr uint8_t TimerInterruptBit = 1u << 0;
    // Board IRQ1 is the keyboard input on the primary controller.
    static constexpr uint8_t KeyboardInterruptBit = 1u << 1;
    // Board IRQ14 is secondary IR6, the primary ATA channel's interrupt input.
    static constexpr uint8_t AtaInterruptBit = 1u << 6;
    struct State {
        std::array<PIC8259, ControllerCount> chips{};
        bool cascadeLine = false, timerPending = false, timerInService = false,
             keyboardInService = false, ataInService = false;
        uint8_t picBase[ControllerCount]{PIC8259::DefaultInterruptVectorBase, SecondaryVectorBase},
            picInit[ControllerCount]{};
    };
    // IMPLEMENTATION NOTE: Restore initial pair state and clear compatibility mirrors.
    // Hardware context: https://www.pcjs.org/documents/datasheets/intel/INTEL_8259A_PIC.pdf#page=19
    void reset() {
        state_ = State{};
    }
    // IMPLEMENTATION NOTE: Expose a chip to machine composition and snapshot code.
    // Hardware context: https://www.pcjs.org/documents/datasheets/intel/INTEL_8259A_PIC.pdf#page=19
    PIC8259 &operator[](size_t index) {
        return state_.chips[index];
    }
    // IMPLEMENTATION NOTE: Expose a chip to machine composition and snapshot code.
    // Hardware context: https://www.pcjs.org/documents/datasheets/intel/INTEL_8259A_PIC.pdf#page=19
    const PIC8259 &operator[](size_t index) const {
        return state_.chips[index];
    }
    // IMPLEMENTATION NOTE: Expose chip iteration for board setup; not a guest operation.
    // Hardware context: https://www.pcjs.org/documents/datasheets/intel/INTEL_8259A_PIC.pdf#page=19
    auto begin() {
        return state_.chips.begin();
    }
    // IMPLEMENTATION NOTE: Return the chip iteration end for board setup; not a guest operation.
    // Hardware context: https://www.pcjs.org/documents/datasheets/intel/INTEL_8259A_PIC.pdf#page=19
    auto end() {
        return state_.chips.end();
    }
    // IMPLEMENTATION NOTE: Expose chip iteration for board setup; not a guest operation.
    // Hardware context: https://www.pcjs.org/documents/datasheets/intel/INTEL_8259A_PIC.pdf#page=19
    auto begin() const {
        return state_.chips.begin();
    }
    // IMPLEMENTATION NOTE: Return the chip iteration end for board setup; not a guest operation.
    // Hardware context: https://www.pcjs.org/documents/datasheets/intel/INTEL_8259A_PIC.pdf#page=19
    auto end() const {
        return state_.chips.end();
    }
    // IMPLEMENTATION NOTE: Expose snapshot/diagnostic state without selecting an interrupt.
    // Hardware context: https://www.pcjs.org/documents/datasheets/intel/INTEL_8259A_PIC.pdf#page=19
    const State &state() const {
        return state_;
    }
    // IMPLEMENTATION NOTE: Route a board IRQ to one of the eight inputs on the appropriate chip.
    // Hardware context: https://www.pcjs.org/documents/datasheets/intel/INTEL_8259A_PIC.pdf#page=19
    void raise_interrupt(unsigned irq) {
        state_.chips[irq >= PIC8259::InterruptInputCount].raise_interrupt(
            irq & PIC8259::InterruptLevelMask);
    }
    // Read IMR or command status on the selected chip.
    uint8_t read_register(unsigned chip, bool data);
    // Route ICW/OCW writes to the selected chip.
    void write_register(unsigned chip, bool data, uint8_t value);
    // IMPLEMENTATION NOTE: Return and acknowledge the next deliverable vector, or no vector.
    // Hardware context: https://www.pcjs.org/documents/datasheets/intel/INTEL_8259A_PIC.pdf#page=19
    std::optional<uint8_t> next_interrupt_vector(bool cpuReady) {
        // Most board polls have no requests on either chip. Keep this path
        // inline while still refreshing compatibility mirrors and clearing a
        // cascade line whose request disappeared. Pending IRQs use the full
        // priority/cascade/acknowledge path, even when the CPU is not ready.
        if (!(state_.chips[PrimaryController].irr | state_.chips[SecondaryController].irr)) {
            state_.cascadeLine = false;
            update_compatibility_mirrors();
            return std::nullopt;
        }
        return acknowledge_pending_vector(cpuReady);
    }

  private:
    friend class SnapshotCodec;
    State state_;
    // IMPLEMENTATION NOTE: Refresh legacy snapshot/diagnostic views of IRR and ISR.
    // Hardware context: https://www.pcjs.org/documents/datasheets/intel/INTEL_8259A_PIC.pdf#page=19
    void update_compatibility_mirrors() {
        state_.timerPending = state_.chips[PrimaryController].irr & TimerInterruptBit;
        state_.timerInService = state_.chips[PrimaryController].isr & TimerInterruptBit;
        state_.keyboardInService = state_.chips[PrimaryController].isr & KeyboardInterruptBit;
        state_.ataInService = state_.chips[SecondaryController].isr & AtaInterruptBit;
    }
    // Resolve and acknowledge requests after the idle fast path misses.
    std::optional<uint8_t> acknowledge_pending_vector(bool cpuReady);
};
} // namespace pc
